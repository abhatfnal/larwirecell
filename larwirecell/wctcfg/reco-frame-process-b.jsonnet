local g = import "pgraph.jsonnet";
local f = import "pgrapher/common/funcs.jsonnet";
local wc = import "wirecell.jsonnet";

local tools_maker = import "pgrapher/common/tools.jsonnet";
local base = import "pgrapher/experiment/sbnd/simparams.jsonnet";
local params = base;
local tools_all = tools_maker(params);
local tools = tools_all { anodes: [tools_all.anodes[n] for n in [0, 1]] };

// Process B receives the already accepted whole-detector IFrame in memory.
local frame_source = g.pnode({
  type: "wclsRecoFrameProcessBSource",
  name: "sigs",
  data: {},
}, nin=0, nout=1);

// These are the archived SBND image and clustering graph builders.  The
// configuration-only MABC shell is replaced below while its exact ordered
// Clus visitor configuration is retained.
local img = import "pgrapher/experiment/sbnd/img.jsonnet";
local img_maker = img();
local img_pipes = [
  img_maker.per_anode(anode, "multi-3view", add_dump=false)
  for anode in tools.anodes
];

local clus = import "pgrapher/experiment/sbnd/clus.jsonnet";
local clus_maker = clus();
// dump=false removes only the downstream TensorFileSink.  MABC remains the
// terminal clustering implementation and its ITensorSet is captured below.
local clus_pipes = [
  clus_maker.per_volume(tools.anodes[0], face=0, dump=false),
  clus_maker.per_volume(tools.anodes[1], face=1, dump=false),
];

local reco_sinks = [
  g.pnode({
    type: "wclsRecoArraysProcessBSink",
    name: "apa%d" % apa,
    data: {
      apa: apa,
      datapath: "pointtrees/%d/live",
      anode: wc.tn(tools.anodes[apa]),
      face: apa,
      geom_helper: "SimpleClusGeomHelper:uboone",
    },
  }, nin=1, nout=0)
  for apa in [0, 1]
];

local captured_clus_pipes = [
  g.pipeline([clus_pipes[apa], reco_sinks[apa]], "captured-clus-apa%d" % apa)
  for apa in [0, 1]
];

local img_clus_pipe = [
  g.intern(
    innodes=[img_pipes[n]],
    centernodes=[],
    outnodes=[captured_clus_pipes[n]],
    edges=[g.edge(img_pipes[n], captured_clus_pipes[n], port, port) for port in std.range(0, 1)],
  )
  for n in std.range(0, std.length(tools.anodes) - 1)
];

local fanout_apa_rules = [
  {
    frame: { ".*": "orig%d" % n },
    trace: {
      gauss: "gauss%d" % n,
      wiener: "wiener%d" % n,
    },
  }
  for n in std.range(0, std.length(tools.anodes) - 1)
];
local parallel_graph =
  f.fanout("FrameFanout", img_clus_pipe, "parallel_graph", fanout_apa_rules);
local graph = g.pipeline([frame_source, parallel_graph], "main");

// pgraph captures endpoint labels when each pnode is constructed.  Keep the
// Pgrapher edge labels synchronized with the configuration-only MABC shell
// replacement below; this changes no ports, ordering or scientific edges.
local archived_mabc_prefix = "MultiAlgBlobClustering:";
local memory_mabc_prefix = "wclsMemoryMultiAlgBlobClustering:";
local memory_node_label(node) =
  if std.substr(node, 0, std.length(archived_mabc_prefix)) == archived_mabc_prefix then
    memory_mabc_prefix + std.substr(
      node,
      std.length(archived_mabc_prefix),
      std.length(node) - std.length(archived_mabc_prefix)
    )
  else node;
local memory_only_edge(edge) = edge {
  tail: edge.tail { node: memory_node_label(edge.tail.node) },
  head: edge.head { node: memory_node_label(edge.head.node) },
};
local app = {
  type: "Pgrapher",
  data: { edges: std.map(memory_only_edge, g.edges(graph)) },
};

local cmdline = {
  type: "wire-cell",
  data: {
    // Same resolved stage-08 scientific plugin set.  Process-B-only factories
    // are linked into the worker executable instead of another WCT plugin.
    plugins: [
      "WireCellGen",
      "WireCellImg",
      "WireCellClus",
      "WireCellPgraph",
      "WireCellSio",
      "WireCellRoot",
      "WireCellLarsoft",
    ],
    apps: ["Pgrapher"],
  },
};

// The archived MABC driver embeds a mandatory Bee::Sink and offers no output-
// disabled switch.  Replace only that orchestration shell with the worker's
// memory-only shell.  This explicit data whitelist preserves the scientific
// input/output paths, geometry, performance touch points and exact ordered
// func_cfgs while ensuring no Bee, grouping2file, JSON or RSE-output option can
// reach the production component.
local memory_only_configuration(component) =
  if component.type == "MultiAlgBlobClustering" then
    component {
      type: "wclsMemoryMultiAlgBlobClustering",
      data: {
        apa: component.data.face,
        inpath: component.data.inpath,
        outpath: component.data.outpath,
        perf: component.data.perf,
        dead_live_overlap_offset: 2,
        anode: component.data.anode,
        face: component.data.face,
        geom_helper: component.data.geom_helper,
        func_cfgs: component.data.func_cfgs,
      },
    }
  else component;

[cmdline] + std.map(memory_only_configuration, g.uses(graph)) + [app]
