#include "third_party/zynamics/bindiff/differ.h"

#include <exception>
#include <iomanip>
#include <memory>

#include "third_party/zynamics/binexport/binexport_header.h"
#include "third_party/zynamics/bindiff/callgraphmatching.h"
#include "third_party/zynamics/bindiff/flowgraphmatching.h"
#include "third_party/zynamics/bindiff/log.h"

#ifdef GOOGLE
#define GOOGLE_PROTOBUF_VERIFY_VERSION
#include "file/base/file.h"
#include "file/base/filesystem.h"
#include "file/base/helpers.h"
#include "file/base/path.h"
#include "net/proto2/io/public/coded_stream.h"
#include "net/proto2/io/public/zero_copy_stream_impl.h"
#include "net/proto2/util/public/google_file_stream.h"
#include "strings/case.h"
#include "third_party/zynamics/binexport/binexport.pb.h"
#include "third_party/zynamics/binexport/binexport2.pb.h"
#include "util/gtl/ptr_util.h"
#include "util/task/canonical_errors.h"
#include "util/task/status.h"
// Google3 uses different namespaces than the open source proto version. This is
// a hack to make them compatible.
namespace google {
namespace protobuf {
namespace io {
using ::proto2::io::IstreamInputStream;
using ::proto2::io::CodedInputStream;
}  // namespace io
}  // namespace protobuf
}  // namespace google
#else
#include "third_party/zynamics/binexport/binexport.pb.h"
#include "third_party/zynamics/binexport/binexport2.pb.h"
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#endif

// Return the immediate children of the call graph node denoted by
// address. Skip nodes that have already been matched.
void GetUnmatchedChildren(const CallGraph& call_graph, CallGraph::Vertex vertex,
                          FlowGraphs* children) {
  CallGraph::OutEdgeIterator edge_it, edge_it_end;
  for (boost::tie(edge_it, edge_it_end) =
           boost::out_edges(vertex, call_graph.GetGraph());
       edge_it != edge_it_end; ++edge_it) {
    if (call_graph.IsDuplicate(*edge_it)) {
      continue;
    }

    const CallGraph::Vertex target =
        boost::target(*edge_it, call_graph.GetGraph());

    FlowGraph* child = call_graph.GetFlowGraph(target);
    if (!child || child->GetFixedPoint()) {
      continue;
    }

    children->insert(child);
  }
}

// Return the immediate parents of the call graph node denoted by
// address. Skip nodes that have already been matched.
void GetUnmatchedParents(const CallGraph& call_graph, CallGraph::Vertex vertex,
                         FlowGraphs* parents) {
  CallGraph::InEdgeIterator edge_it, edge_it_end;
  for (boost::tie(edge_it, edge_it_end) =
           boost::in_edges(vertex, call_graph.GetGraph());
       edge_it != edge_it_end; ++edge_it) {
    if (call_graph.IsDuplicate(*edge_it)) {
      continue;
    }

    const CallGraph::Vertex source =
        boost::source(*edge_it, call_graph.GetGraph());

    FlowGraph* parent = call_graph.GetFlowGraph(source);
    if (!parent || parent->GetFixedPoint()) {
      continue;
    }

    parents->insert(parent);
  }
}

// This adds empty flow graphs for functions imported from dlls.
void AddSubsToCallGraph(CallGraph* call_graph, FlowGraphs* flow_graphs) {
  CallGraph::VertexIterator i, end;
  for (boost::tie(i, end) = boost::vertices(call_graph->GetGraph()); i != end;
       ++i) {
    const CallGraph::Vertex vertex = *i;
    const Address address = call_graph->GetAddress(vertex);
    FlowGraph* flow_graph = call_graph->GetFlowGraph(vertex);
    if (flow_graph) {
      continue;
    }

    flow_graph = new FlowGraph(call_graph, address);
    call_graph->SetStub(vertex, true);
    call_graph->SetLibrary(vertex, true);
    CHECK(flow_graphs->insert(flow_graph).second);
  }
}

#ifdef GOOGLE
bool ReadBinExport2Google(File* file, CallGraph* call_graph,
                          FlowGraphs* flow_graphs,
                          FlowGraphInfos* flow_graph_infos,
                          Instruction::Cache* instruction_cache) {
  call_graph->Reset();
  DeleteFlowGraphs(flow_graphs);
  flow_graph_infos->clear();

  proto2::util::GoogleFileInputStream stream(file);
  // We explicitly instantiate our own decoder instead of using the proto's
  // convenience methods in order to be able to set the bytes limit thresholds.
  // Our protos are typically very large, and the default limit warns at 32MB.
  proto2::io::CodedInputStream decoder(&stream);
  decoder.SetTotalBytesLimit(std::numeric_limits<int32_t>::max(),
                             std::numeric_limits<int32_t>::max());
  BinExport2 proto;
  if (!proto.ParseFromCodedStream(&decoder) ||
      !decoder.ConsumedEntireMessage()) {
    return false;
  }

  call_graph->Read(proto, file->filename());
  for (const auto& proto_flow_graph : proto.flow_graph()) {
    if (proto_flow_graph.basic_block_index_size() == 0) {
      LOG(WARNING) << "Skipping empty flow graph!";
      continue;
    }
    auto flow_graph = ::util::gtl::MakeUnique<FlowGraph>();
    flow_graph->Read(proto, proto_flow_graph, call_graph, instruction_cache);
    flow_graphs->insert(flow_graph.release());
  }

  AddSubsToCallGraph(call_graph, flow_graphs);
  return true;
}

bool ReadBinExportGoogle(File* file, CallGraph* call_graph,
                         FlowGraphs* flow_graphs,
                         FlowGraphInfos* flow_graph_infos,
                         Instruction::Cache* instruction_cache) {
  call_graph->Reset();
  DeleteFlowGraphs(flow_graphs);
  flow_graph_infos->clear();

  auto status_or(BinExportHeader::ParseFromFile(file));
  if (!status_or.ok()) {
    return false;
  }
  auto header(status_or.ConsumeValueOrDie());
  {
    const uint32_t size = header.call_graph_offset - header.meta_offset;
    QCHECK_LT(size, 500000000) << file->filename();
    string buffer(size, '\0');
    QCHECK_OK(
        file::ReadToBuffer(file, &buffer[0], size, nullptr, file::Defaults()))
        << file->filename();
    BinExport::Meta meta_information;
    QCHECK(meta_information.ParseFromString(buffer)) << file->filename();
    call_graph->SetExeFilename(meta_information.input_binary());
    call_graph->SetExeHash(meta_information.input_hash());
  }
  {
    const uint32_t size =
        header.flow_graph_offsets[0].offset - header.call_graph_offset;
    QCHECK_LT(size, 500000000) << file->filename();
    string buffer(size, '\0');
    CHECK_OK(
        file::ReadToBuffer(file, &buffer[0], size, nullptr, file::Defaults()))
        << file->filename();
    BinExport::Callgraph call_graph_proto;
    QCHECK(call_graph_proto.ParseFromString(buffer)) << file->filename();
    call_graph->Read(call_graph_proto, file->filename());
  }

  string buffer;
  for (uint32_t i = 0; i < header.num_flow_graphs; ++i) {
    // Note the +1 index is safe because we add an artificial
    // entry into the header flow graph table.
    const uint32_t size = header.flow_graph_offsets[i + 1].offset -
                          header.flow_graph_offsets[i].offset;
    QCHECK_LT(size, 500000000) << file->filename();
    buffer.resize(size);
    QCHECK_OK(
        file::ReadToBuffer(file, &buffer[0], size, nullptr, file::Defaults()))
        << file->filename();
    BinExport::Flowgraph flow_graph_proto;
    QCHECK(flow_graph_proto.ParseFromString(buffer)) << file->filename();
    if (flow_graph_proto.vertices_size() == 0) {
      LOG(WARNING) << "Skipping empty flow graph! " << std::hex
                   << flow_graph_proto.address();
      continue;
    }
    auto flow_graph_owner = ::util::gtl::MakeUnique<FlowGraph>(
        call_graph, static_cast<Address>(flow_graph_proto.address()));
    auto flow_graph = flow_graph_owner.get();
    flow_graph_owner->Read(flow_graph_proto, instruction_cache);
    QCHECK(flow_graphs->insert(flow_graph_owner.release()).second)
        << file->filename();

    Counts counts;
    Count(*flow_graph, &counts);
    FlowGraphInfo info;
    info.address = flow_graph->GetEntryPointAddress();
    info.file_offset = header.flow_graph_offsets[i].offset;
    info.name = &flow_graph->GetName();
    info.demangled_name = &flow_graph->GetDemangledName();
    info.basic_block_count =
        counts["basicBlocks (library)"] + counts["basicBlocks (non-library)"];
    info.edge_count = counts["edges (library)"] + counts["edges (non-library)"];
    info.instruction_count =
        counts["instructions (library)"] + counts["instructions (non-library)"];
    (*flow_graph_infos)[info.address] = info;
  }
  AddSubsToCallGraph(call_graph, flow_graphs);
  return true;
}

bool ReadGoogle(const std::string& filename, CallGraph* call_graph,
                FlowGraphs* flow_graphs, FlowGraphInfos* flow_graph_infos,
                Instruction::Cache* instruction_cache) {
  GOOGLE_PROTOBUF_VERIFY_VERSION;

  FileCloser file(File::Open(filename.c_str(), "r"));
  if (!file.get()) {
    if (util::IsNotFound(file::Exists(filename, file::Defaults()))) {
      LOG(ERROR) << "Error opening '" << filename << "', file doesn't exist.";
    } else {
      LOG(ERROR) << "Error opening '" << filename << "'.";
    }
    return false;
  }

  if (!ReadBinExport2Google(file.get(), call_graph, flow_graphs,
                            flow_graph_infos, instruction_cache)) {
    file->Seek(0 /* Position */, file::Defaults()).IgnoreError();
    return ReadBinExportGoogle(file.get(), call_graph, flow_graphs,
                               flow_graph_infos, instruction_cache);
  }
  return true;
}
#endif  // GOOGLE

bool ReadBinExport2(const std::string& filename, CallGraph* call_graph,
                    FlowGraphs* flow_graphs, FlowGraphInfos* flow_graph_infos,
                    Instruction::Cache* instruction_cache) {
  call_graph->Reset();
  DeleteFlowGraphs(flow_graphs);
  flow_graph_infos->clear();

  std::ifstream stream(filename, std::ios::binary);
  BinExport2 proto;
  if (!proto.ParseFromIstream(&stream)) {
    return false;
  }

  call_graph->Read(proto, filename);
  for (const auto& proto_flow_graph : proto.flow_graph()) {
    std::unique_ptr<FlowGraph> flow_graph(new FlowGraph());
    flow_graph->Read(proto, proto_flow_graph, call_graph, instruction_cache);
    flow_graphs->insert(flow_graph.release());
  }

  AddSubsToCallGraph(call_graph, flow_graphs);
  return true;
}

void ReadBinExport(const std::string& filename, CallGraph* call_graph,
                   FlowGraphs* flow_graphs, FlowGraphInfos* flow_graph_infos,
                   Instruction::Cache* instruction_cache) {
  call_graph->Reset();
  DeleteFlowGraphs(flow_graphs);
  flow_graph_infos->clear();

  enum {
    kBytesLimit = 2147483647 /* 2GiB less one byte */
  };

  std::ifstream file(filename.c_str(), std::ios_base::binary);
  if (!file) {
    throw std::runtime_error(("failed reading \"" + filename + "\"").c_str());
  }
  auto header(BinExportHeader::ParseFromStream(&file));
  google::protobuf::io::IstreamInputStream stream(&file);
  google::protobuf::io::CodedInputStream::Limit limit;
  {
    google::protobuf::io::CodedInputStream coded_stream(&stream);
    coded_stream.SetTotalBytesLimit(kBytesLimit, -1 /* No warning */);
    BinExport::Meta meta_information;
    limit =
        coded_stream.PushLimit(header.call_graph_offset - header.meta_offset);
    if (!meta_information.ParseFromCodedStream(&coded_stream)) {
      throw std::runtime_error(
          ("failed to parse meta data in \"" + filename + "\"").c_str());
    }
    coded_stream.PopLimit(limit);
    call_graph->SetExeFilename(meta_information.input_binary());
    call_graph->SetExeHash(meta_information.input_hash());
    BinExport::Callgraph call_graph_proto;
    limit = coded_stream.PushLimit(header.flow_graph_offsets[0].offset -
                                   header.call_graph_offset);
    if (!call_graph_proto.ParseFromCodedStream(&coded_stream)) {
      throw std::runtime_error(
          ("failed to parse call graph data in \"" + filename + "\"").c_str());
    }
    coded_stream.PopLimit(limit);
    call_graph->Read(call_graph_proto, filename);
  }

  BinExport::Flowgraph flow_graph_proto;
  for (uint32_t i = 0; i < header.num_flow_graphs; ++i) {
    // It may seem strange to recreate the input stream per protocol message.
    // However, it turned out that this is required for very large streams. Even
    // if I set the total bytes limit to >1GB we'd terminate if we had read an
    // aggregate message size of ~1GB (individual messages are way smaller!).
    // The current solution is efficient and was proposed here:
    // https://groups.google.com/d/msg/protobuf/ZKB5EePJDNU/NbDd2tctgVYJ
    google::protobuf::io::CodedInputStream coded_stream(&stream);
    coded_stream.SetTotalBytesLimit(kBytesLimit, -1 /* No warning */);
    // Note the +1 index is safe because we add an artificial
    // entry into the header flow graph table.
    limit = coded_stream.PushLimit(header.flow_graph_offsets[i + 1].offset -
                                   header.flow_graph_offsets[i].offset);
    if (!flow_graph_proto.ParseFromCodedStream(&coded_stream)) {
      throw std::runtime_error(("failed reading \"" + filename + "\"").c_str());
    }
    coded_stream.PopLimit(limit);
    std::unique_ptr<FlowGraph> flow_graph_owner(new FlowGraph(
        call_graph, static_cast<Address>(flow_graph_proto.address())));
    auto* flow_graph = flow_graph_owner.get();
    flow_graph->Read(flow_graph_proto, instruction_cache);
    CHECK(flow_graphs->insert(flow_graph_owner.release()).second);

    Counts counts;
    Count(*flow_graph, &counts);
    FlowGraphInfo info;
    info.address = flow_graph->GetEntryPointAddress();
    info.file_offset = header.flow_graph_offsets[i].offset;
    info.name = &flow_graph->GetName();
    info.demangled_name = &flow_graph->GetDemangledName();
    info.basic_block_count =
        counts["basicBlocks (library)"] + counts["basicBlocks (non-library)"];
    info.edge_count = counts["edges (library)"] + counts["edges (non-library)"];
    info.instruction_count =
        counts["instructions (library)"] + counts["instructions (non-library)"];
    (*flow_graph_infos)[info.address] = info;
  }

  AddSubsToCallGraph(call_graph, flow_graphs);
}

void Read(const std::string& filename, CallGraph* call_graph,
          FlowGraphs* flow_graphs, FlowGraphInfos* flow_graph_infos,
          Instruction::Cache* instruction_cache) {
  GOOGLE_PROTOBUF_VERIFY_VERSION;
  LOG(INFO) << "Reading: " << filename;
  if (!ReadBinExport2(filename, call_graph, flow_graphs, flow_graph_infos,
                      instruction_cache)) {
    ReadBinExport(filename, call_graph, flow_graphs, flow_graph_infos,
                  instruction_cache);
  }
}

void DeleteFlowGraphs(FlowGraphs* flow_graphs) {
  if (!flow_graphs) {
    return;
  }

  for (auto* flow_graph : *flow_graphs) {
    delete flow_graph;
  }
  flow_graphs->clear();
}

ScopedCleanup::ScopedCleanup(FlowGraphs* flow_graphs1, FlowGraphs* flow_graphs2,
                             Instruction::Cache* instruction_cache)
    : flow_graphs1_(flow_graphs1),
      flow_graphs2_(flow_graphs2),
      instruction_cache_(instruction_cache) {}

ScopedCleanup::~ScopedCleanup() {
  DeleteFlowGraphs(flow_graphs1_);
  DeleteFlowGraphs(flow_graphs2_);
  if (instruction_cache_) {
    instruction_cache_->Clear();
  }
}

void ResetMatches(FlowGraphs* flow_graphs) {
  for (auto* flow_graph : *flow_graphs) {
    flow_graph->ResetMatches();
  }
}

void Diff(MatchingContext* context,
          const MatchingSteps& default_call_graph_steps,
          const MatchingStepsFlowGraph& default_basic_block_steps) {
  // The outer loop controls the rigorousness for initial matching while the
  // inner loop tries to resolve ambiguities by drilling down the matchingSteps
  // lists.
  for (MatchingSteps matching_steps_for_current_level =
           default_call_graph_steps;
       !matching_steps_for_current_level.empty();
       matching_steps_for_current_level.pop_front()) {
    context->new_fixed_points_.clear();
    MatchingSteps matching_steps = matching_steps_for_current_level;
    MatchingStep* step = matching_steps.front();
    step->FindFixedPoints(
        nullptr /* primary_parent */, nullptr /* secondary_parent */,
        context->primary_flow_graphs_, context->secondary_flow_graphs_,
        *context, matching_steps, default_basic_block_steps);
    matching_steps = matching_steps_for_current_level;

    bool more_fixed_points_discovered = false;
    do {
      more_fixed_points_discovered = false;

      // Performance: We iterate over _all_ fixed points discovered so far. The
      // idea being that parents/children that previously lead to ambiguous
      // matches may now be unique after some of their siblings have been
      // matched. This is expensive and we may want to iterate new fixed points
      // only instead?
      // Propagate down to the children of the new fixed points.
      for (const auto& fixed_point : context->fixed_points_) {
        matching_steps = matching_steps_for_current_level;
        FlowGraphs primary_children, secondary_children;
        GetUnmatchedChildren(context->primary_call_graph_,
                             fixed_point.GetPrimary()->GetCallGraphVertex(),
                             &primary_children);
        GetUnmatchedChildren(context->secondary_call_graph_,
                             fixed_point.GetSecondary()->GetCallGraphVertex(),
                             &secondary_children);
        if (!primary_children.empty() && !secondary_children.empty()) {
          more_fixed_points_discovered |= step->FindFixedPoints(
              fixed_point.GetPrimary(), fixed_point.GetSecondary(),
              primary_children, secondary_children, *context, matching_steps,
              default_basic_block_steps);
        }
      }

      // Propagate up to the parents of the new fixed points.
      for (const auto& fixed_point : context->fixed_points_) {
        matching_steps = matching_steps_for_current_level;
        FlowGraphs primary_parents, secondary_parents;
        GetUnmatchedParents(context->primary_call_graph_,
                            fixed_point.GetPrimary()->GetCallGraphVertex(),
                            &primary_parents);
        GetUnmatchedParents(context->secondary_call_graph_,
                            fixed_point.GetSecondary()->GetCallGraphVertex(),
                            &secondary_parents);
        if (!primary_parents.empty() && !secondary_parents.empty()) {
          more_fixed_points_discovered |= step->FindFixedPoints(
              fixed_point.GetPrimary(), fixed_point.GetSecondary(),
              primary_parents, secondary_parents, *context, matching_steps,
              default_basic_block_steps);
        }
      }
    } while (more_fixed_points_discovered);

    // After collecting initial fixed points for this step: iterate over all of
    // them and find call reference fixed points.
    for (auto* fixed_point : context->new_fixed_points_) {
      FindCallReferenceFixedPoints(fixed_point, context,
                                   default_basic_block_steps);
    }
  }
  ClassifyChanges(context);
}

void Count(const FlowGraph& flow_graph, Counts* counts) {
  FlowGraphs flow_graphs;
  CHECK(flow_graphs.insert(&const_cast<FlowGraph&>(flow_graph)).second);
  Count(flow_graphs, counts);
}

void Count(const FlowGraphs& flow_graphs, Counts* counts) {
  Counts::mapped_type num_functions = 0;
  Counts::mapped_type num_basic_blocks = 0;
  Counts::mapped_type num_instructions = 0;
  Counts::mapped_type num_edges = 0;
  Counts::mapped_type num_lib_functions = 0;
  Counts::mapped_type num_lib_basic_blocks = 0;
  Counts::mapped_type num_lib_instructions = 0;
  Counts::mapped_type num_lib_edges = 0;
  for (const FlowGraph* flow_graph : flow_graphs) {
    Counts::mapped_type& basic_blocks =
        flow_graph->IsLibrary() ? num_lib_basic_blocks : num_basic_blocks;
    Counts::mapped_type& instructions =
        flow_graph->IsLibrary() ? num_lib_instructions : num_instructions;
    Counts::mapped_type& edges =
        flow_graph->IsLibrary() ? num_lib_edges : num_edges;
    num_functions += 1 - flow_graph->IsLibrary();
    num_lib_functions += flow_graph->IsLibrary();

    FlowGraph::VertexIterator j, end;
    for (boost::tie(j, end) = boost::vertices(flow_graph->GetGraph()); j != end;
         ++j) {
      ++basic_blocks;
      instructions += flow_graph->GetInstructionCount(*j);
    }
    edges += boost::num_edges(flow_graph->GetGraph());
  }

  (*counts)["functions (library)"] = num_lib_functions;
  (*counts)["functions (non-library)"] = num_functions;
  (*counts)["basicBlocks (library)"] = num_lib_basic_blocks;
  (*counts)["basicBlocks (non-library)"] = num_basic_blocks;
  (*counts)["instructions (library)"] = num_lib_instructions;
  (*counts)["instructions (non-library)"] = num_instructions;
  (*counts)["edges (library)"] = num_lib_edges;
  (*counts)["edges (non-library)"] = num_edges;
}

void Count(const FixedPoint& fixed_point, Counts* counts,
           Histogram* histogram) {
  (*counts)["function matches (library)"] = 0;
  (*counts)["basicBlock matches (library)"] = 0;
  (*counts)["instruction matches (library)"] = 0;
  (*counts)["flowGraph edge matches (library)"] = 0;
  (*counts)["function matches (non-library)"] = 0;
  (*counts)["basicBlock matches (non-library)"] = 0;
  (*counts)["instruction matches (non-library)"] = 0;
  (*counts)["flowGraph edge matches (non-library)"] = 0;

  const FlowGraph* primary = fixed_point.GetPrimary();
  const FlowGraph* secondary = fixed_point.GetSecondary();
  const bool library = primary->IsLibrary() || secondary->IsLibrary();
  Counts::mapped_type& functions(
      library ? (*counts)["function matches (library)"]
              : (*counts)["function matches (non-library)"]);
  Counts::mapped_type& basic_blocks(
      library ? (*counts)["basicBlock matches (library)"]
              : (*counts)["basicBlock matches (non-library)"]);
  Counts::mapped_type& instructions(
      library ? (*counts)["instruction matches (library)"]
              : (*counts)["instruction matches (non-library)"]);
  Counts::mapped_type& edges(
      library ? (*counts)["flowGraph edge matches (library)"]
              : (*counts)["flowGraph edge matches (non-library)"]);

  (*histogram)[fixed_point.GetMatchingStep()]++;
  functions++;
  basic_blocks += fixed_point.GetBasicBlockFixedPoints().size();
  for (auto it = fixed_point.GetBasicBlockFixedPoints().cbegin();
       it != fixed_point.GetBasicBlockFixedPoints().cend(); ++it) {
    (*histogram)[it->GetMatchingStep()]++;
    instructions += it->GetInstructionMatches().size();
  }

  FlowGraph::EdgeIterator j, jend;
  for (boost::tie(j, jend) = boost::edges(primary->GetGraph()); j != jend;
       ++j) {
    const auto source1 = boost::source(*j, primary->GetGraph());
    const auto target1 = boost::target(*j, primary->GetGraph());
    // Source and target basic blocks are matched, check whether there's an
    // edge connecting the two.
    if (primary->GetFixedPoint(source1) && primary->GetFixedPoint(target1)) {
      const auto source2 =
          primary->GetFixedPoint(source1)->GetSecondaryVertex();
      const Address target2 =
          primary->GetFixedPoint(target1)->GetSecondaryVertex();
      // Both are in secondary graph as well.
      FlowGraph::OutEdgeIterator k, kend;
      for (boost::tie(k, kend) =
               boost::out_edges(source2, secondary->GetGraph());
           k != kend; ++k) {
        if (boost::target(*k, secondary->GetGraph()) == target2) {
          ++edges;
          break;
        }
      }
    }
  }
}

// TODO(soerenme) remove the statics!!! threading issues!
double GetConfidence(const Histogram& histogram, Confidences* confidences) {
  {
    static MatchingSteps steps = GetDefaultMatchingSteps();
    for (auto i = steps.cbegin(); i != steps.cend(); ++i) {
      (*confidences)[(*i)->GetName()] = (*i)->GetConfidence();
    }
  }
  {
    static MatchingStepsFlowGraph steps = GetDefaultMatchingStepsBasicBlock();
    for (auto i = steps.cbegin(); i != steps.cend(); ++i) {
      (*confidences)[(*i)->GetName()] = (*i)->GetConfidence();
    }
  }
  (*confidences)["basicBlock: propagation (size==1)"] = 0.0;
  (*confidences)["function: call reference matching"] = 0.75;
  double confidence = 0.0;
  double match_count = 0;
  for (auto i = histogram.cbegin(); i != histogram.cend(); ++i) {
    confidence += i->second * (*confidences)[i->first];
    match_count += i->second;
  }
  // sigmoid squashing function
  return match_count
             ? 1.0 / (1.0 + exp(-(confidence / match_count - 0.5) * 10.0))
             : 0.0;
}

void GetCountsAndHistogram(const FlowGraphs& flow_graphs1,
                           const FlowGraphs& flow_graphs2,
                           const FixedPoints& fixed_points,
                           Histogram* histogram, Counts* counts) {
  Counts counts1, counts2;
  Count(flow_graphs1, &counts1);
  Count(flow_graphs2, &counts2);

  (*counts)["functions primary (library)"] = counts1["functions (library)"];
  (*counts)["functions primary (non-library)"] =
      counts1["functions (non-library)"];
  (*counts)["functions secondary (library)"] = counts2["functions (library)"];
  (*counts)["functions secondary (non-library)"] =
      counts2["functions (non-library)"];
  (*counts)["basicBlocks primary (library)"] = counts1["basicBlocks (library)"];
  (*counts)["basicBlocks primary (non-library)"] =
      counts1["basicBlocks (non-library)"];
  (*counts)["basicBlocks secondary (library)"] =
      counts2["basicBlocks (library)"];
  (*counts)["basicBlocks secondary (non-library)"] =
      counts2["basicBlocks (non-library)"];

  (*counts)["instructions primary (library)"] =
      counts1["instructions (library)"];
  (*counts)["instructions primary (non-library)"] =
      counts1["instructions (non-library)"];
  (*counts)["instructions secondary (library)"] =
      counts2["instructions (library)"];
  (*counts)["instructions secondary (non-library)"] =
      counts2["instructions (non-library)"];

  (*counts)["flowGraph edges primary (library)"] = counts1["edges (library)"];
  (*counts)["flowGraph edges primary (non-library)"] =
      counts1["edges (non-library)"];
  (*counts)["flowGraph edges secondary (library)"] = counts2["edges (library)"];
  (*counts)["flowGraph edges secondary (non-library)"] =
      counts2["edges (non-library)"];

  (*counts)["function matches (library)"] = 0;
  (*counts)["basicBlock matches (library)"] = 0;
  (*counts)["instruction matches (library)"] = 0;
  (*counts)["flowGraph edge matches (library)"] = 0;
  (*counts)["function matches (non-library)"] = 0;
  (*counts)["basicBlock matches (non-library)"] = 0;
  (*counts)["instruction matches (non-library)"] = 0;
  (*counts)["flowGraph edge matches (non-library)"] = 0;
  for (auto i = fixed_points.cbegin(), end = fixed_points.cend(); i != end;
       ++i) {
    Counts fixed_point_counts;
    Count(*i, &fixed_point_counts, histogram);
    (*counts)["function matches (library)"] +=
        fixed_point_counts["function matches (library)"];
    (*counts)["basicBlock matches (library)"] +=
        fixed_point_counts["basicBlock matches (library)"];
    (*counts)["instruction matches (library)"] +=
        fixed_point_counts["instruction matches (library)"];
    (*counts)["flowGraph edge matches (library)"] +=
        fixed_point_counts["flowGraph edge matches (library)"];
    (*counts)["function matches (non-library)"] +=
        fixed_point_counts["function matches (non-library)"];
    (*counts)["basicBlock matches (non-library)"] +=
        fixed_point_counts["basicBlock matches (non-library)"];
    (*counts)["instruction matches (non-library)"] +=
        fixed_point_counts["instruction matches (non-library)"];
    (*counts)["flowGraph edge matches (non-library)"] +=
        fixed_point_counts["flowGraph edge matches (non-library)"];
  }
}

// Flow graph similarity includes library functions.
double GetSimilarityScore(const FlowGraph& flow_graph1,
                          const FlowGraph& flow_graph2,
                          const Histogram& histogram, const Counts& counts) {
  const int basic_block_matches =
      counts.find("basicBlock matches (non-library)")->second +
      counts.find("basicBlock matches (library)")->second;
  const int basic_blocks_primary =
      counts.find("basicBlocks primary (non-library)")->second +
      counts.find("basicBlocks primary (library)")->second;
  const int basic_blocks_secondary =
      counts.find("basicBlocks secondary (non-library)")->second +
      counts.find("basicBlocks secondary (library)")->second;
  const int instruction_matches =
      counts.find("instruction matches (non-library)")->second +
      counts.find("instruction matches (library)")->second;
  const int instructions_primary =
      counts.find("instructions primary (non-library)")->second +
      counts.find("instructions primary (library)")->second;
  const int instructions_secondary =
      counts.find("instructions secondary (non-library)")->second +
      counts.find("instructions secondary (library)")->second;
  const int edge_matches =
      counts.find("flowGraph edge matches (non-library)")->second +
      counts.find("flowGraph edge matches (library)")->second;
  const int edges_primary =
      counts.find("flowGraph edges primary (non-library)")->second +
      counts.find("flowGraph edges primary (library)")->second;
  const int edges_secondary =
      counts.find("flowGraph edges secondary (non-library)")->second +
      counts.find("flowGraph edges secondary (library)")->second;

  if (basic_block_matches == basic_blocks_primary &&
      basic_block_matches == basic_blocks_secondary &&
      instruction_matches == instructions_primary &&
      instruction_matches == instructions_secondary) {
    return 1.0;
  }

  double similarity = 0;
  similarity += 0.55 * edge_matches /
                (std::max(1.0, 0.5 * (edges_primary + edges_secondary)));
  similarity +=
      0.30 * basic_block_matches /
      (std::max(1.0, 0.5 * (basic_blocks_primary + basic_blocks_secondary)));
  similarity +=
      0.15 * instruction_matches /
      (std::max(1.0, 0.5 * (instructions_primary + instructions_secondary)));
  similarity = std::min(similarity, 1.0);
  similarity += 1.0 -
                std::fabs(flow_graph1.GetMdIndex() - flow_graph2.GetMdIndex()) /
                    (1.0 + flow_graph1.GetMdIndex() + flow_graph2.GetMdIndex());
  similarity /= 2.0;

  // TODO(soerenme) Investigate this:
  //     Disable this because a 1.0 match gets voted down due to low confidence.
  Confidences confidences;
  similarity *= GetConfidence(histogram, &confidences);
  return similarity;
}

// Global similarity score excludes library functions so these won't
// inflate our similarity score.
double GetSimilarityScore(const CallGraph& call_graph1,
                          const CallGraph& call_graph2,
                          const Histogram& histogram, const Counts& counts) {
  double similarity = 0;
  similarity +=
      0.35 * counts.find("flowGraph edge matches (non-library)")->second /
      (std::max(
          1.0,
          0.5 * (counts.find("flowGraph edges primary (non-library)")->second +
                 counts.find("flowGraph edges secondary (non-library)")
                     ->second)));
  similarity +=
      0.25 * counts.find("basicBlock matches (non-library)")->second /
      (std::max(
          1.0,
          0.5 * (counts.find("basicBlocks primary (non-library)")->second +
                 counts.find("basicBlocks secondary (non-library)")->second)));
  similarity +=
      0.10 * counts.find("function matches (non-library)")->second /
      (std::max(
          1.0,
          0.5 * (counts.find("functions primary (non-library)")->second +
                 counts.find("functions secondary (non-library)")->second)));
  similarity +=
      0.10 * counts.find("instruction matches (non-library)")->second /
      (std::max(
          1.0,
          0.5 * (counts.find("instructions primary (non-library)")->second +
                 counts.find("instructions secondary (non-library)")->second)));
  similarity +=
      0.20 * (1.0 -
              std::fabs(call_graph1.GetMdIndex() - call_graph2.GetMdIndex()) /
                  (1.0 + call_graph1.GetMdIndex() + call_graph2.GetMdIndex()));
  similarity = std::min(similarity, 1.0);

  Confidences confidences;
  similarity *= GetConfidence(histogram, &confidences);
  return similarity;
}
