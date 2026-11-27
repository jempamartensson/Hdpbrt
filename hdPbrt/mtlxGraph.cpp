// Procedural displacement (pbrt patch 41): compiles the MaterialX graph that
// drives a displacement node into pbrt's hdpbrtgraph register program
// (src/pbrt/util/hdpbrtgraph.h in the pbrt fork). Nodes are implemented
// directly (math, geometry, noise, image) or expanded through their stdlib
// nodegraph; unauthored inputs take the nodedef defaults (values or default
// geomprops), all from mtlxStdlib.inc (tools/make_mtlx_defaults.py).

#include "hdPbrt.h"

#include "pxr/usd/sdf/assetPath.h"
#include "pxr/base/gf/vec2f.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/gf/vec4f.h"
#include "pxr/base/tf/stringUtils.h"

#include <array>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <unordered_map>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

struct PortRow {
    const char *nodedef, *node, *port, *type, *value, *geomprop;
    bool isOutput;
};
const PortRow kPorts[] = {
#define HDPBRT_MTLX_PORTS
#include "mtlxStdlib.inc"
#undef HDPBRT_MTLX_PORTS
};
struct GraphRow {
    const char *nodedef, *node, *category, *type, *input, *value, *nodename, *output,
        *interfacename;
};
const GraphRow kGraphs[] = {
#define HDPBRT_MTLX_GRAPHS
#include "mtlxStdlib.inc"
#undef HDPBRT_MTLX_GRAPHS
};

struct Port {
    std::string name, type, value, geomprop;
};
struct NodeDef {
    std::string name, category;
    std::vector<Port> inputs, outputs;
    const Port *Input(const std::string &n) const {
        for (const Port &p : inputs)
            if (p.name == n)
                return &p;
        return nullptr;
    }
};
struct GInput {
    std::string name, type, value, nodename, output, interfacename;
};
struct GNode {
    std::string name, category, type;
    std::vector<GInput> inputs;
};
struct GOutput {
    std::string name, type, nodename, output;
};
struct NodeGraph {
    std::vector<GNode> nodes;
    std::vector<GOutput> outputs;
    const GNode *Node(const std::string &n) const {
        for (const GNode &g : nodes)
            if (g.name == n)
                return &g;
        return nullptr;
    }
};

struct Library {
    std::unordered_map<std::string, NodeDef> defs;
    std::unordered_map<std::string, std::vector<const NodeDef *>> byCategory;
    std::unordered_map<std::string, NodeGraph> graphs;  // by nodedef
};

const Library &Lib()
{
    static const Library lib = [] {
        Library l;
        for (const PortRow &r : kPorts) {
            NodeDef &d = l.defs[r.nodedef];
            d.name = r.nodedef;
            d.category = r.node;
            (r.isOutput ? d.outputs : d.inputs).push_back({r.port, r.type, r.value, r.geomprop});
        }
        for (auto &kv : l.defs)
            l.byCategory[kv.second.category].push_back(&kv.second);
        for (const GraphRow &r : kGraphs) {
            NodeGraph &g = l.graphs[r.nodedef];
            if (!*r.node) {  // graph output
                g.outputs.push_back({r.input, r.type, r.nodename, r.output});
            } else if (*r.category) {  // node row
                g.nodes.push_back({r.node, r.category, r.type, {}});
            } else if (!g.nodes.empty()) {
                g.nodes.back().inputs.push_back(
                    {r.input, r.type, r.value, r.nodename, r.output, r.interfacename});
            }
        }
        return l;
    }();
    return lib;
}

int Width(const std::string &type)
{
    if (type == "vector2") return 2;
    if (type == "vector3" || type == "color3") return 3;
    if (type == "vector4" || type == "color4") return 4;
    return 1;
}

bool IsNumeric(const std::string &type)
{
    return type == "float" || type == "integer" || type == "boolean" || type == "vector2" ||
           type == "vector3" || type == "color3" || type == "vector4" || type == "color4";
}

using V4 = std::array<float, 4>;

// A compiled value: a register, or a string (uniform string/filename input).
struct Val {
    int reg = -1;
    int width = 1;
    bool isString = false;
    std::string str;
    bool isConst = false;
    V4 constVal{0, 0, 0, 0};
    bool ok() const { return isString || reg >= 0; }
};

struct CompileError {
    std::string what;
};

[[noreturn]] void Fail(const std::string &what)
{
    throw CompileError{what};
}

V4 ParseValue(const std::string &s, const std::string &type)
{
    if (type == "boolean")
        return s == "true" ? V4{1, 1, 1, 1} : V4{0, 0, 0, 0};
    std::vector<float> f;
    for (const std::string &t : TfStringSplit(s, ","))
        f.push_back(float(std::atof(t.c_str())));
    if (f.empty())
        f.push_back(0.f);
    if (f.size() == 1)
        return V4{f[0], f[0], f[0], f[0]};
    V4 v{0, 0, 0, 0};
    for (size_t i = 0; i < f.size() && i < 4; ++i)
        v[i] = f[i];
    return v;
}

class Compiler {
  public:
    Compiler(const HdMaterialNetwork2 &net, HdPbrtRenderDelegate *delegate)
        : _net(net), _delegate(delegate) {}

    std::ostringstream code;
    std::vector<std::string> images;
    std::vector<std::pair<std::string, int>> primvars;

    int PrimvarSlot(const std::string &name, int width)
    {
        for (size_t i = 0; i < primvars.size(); ++i)
            if (primvars[i].first == name && primvars[i].second == width)
                return int(i);
        primvars.push_back({name, width});
        return int(primvars.size() - 1);
    }
    int nextReg = 0;

    int NewReg() { return nextReg++; }

    Val Const(V4 v, int width)
    {
        auto it = _consts.find(v);
        Val r;
        r.width = width;
        r.isConst = true;
        r.constVal = v;
        if (it != _consts.end()) {
            r.reg = it->second;
            return r;
        }
        r.reg = NewReg();
        _consts[v] = r.reg;
        code << "c " << r.reg << " " << v[0] << " " << v[1] << " " << v[2] << " " << v[3]
             << ";";
        return r;
    }
    Val ConstF(float f) { return Const(V4{f, f, f, f}, 1); }

    Val Emit(const char *op, std::initializer_list<int> args, int width)
    {
        Val r;
        r.reg = NewReg();
        r.width = width;
        code << op << " " << r.reg;
        for (int a : args)
            code << " " << a;
        code << ";";
        return r;
    }

    // ---- Hydra nodes --------------------------------------------------------

    using Resolver = std::function<Val(const std::string &)>;

    Val HydraOutput(const HdMaterialNode2 &node, const std::string &output)
    {
        auto key = std::make_pair((const void *)&node, output);
        auto it = _memo.find(key);
        if (it != _memo.end())
            return it->second;
        const std::string nodedef = node.nodeTypeId.GetString();
        auto d = Lib().defs.find(nodedef);
        if (d == Lib().defs.end())
            Fail("unknown node " + nodedef);
        Resolver in = [this, &node, def = &d->second](const std::string &name) {
            auto c = node.inputConnections.find(TfToken(name));
            if (c != node.inputConnections.end() && !c->second.empty()) {
                const HdMaterialConnection2 &conn = c->second.front();
                auto up = _net.nodes.find(conn.upstreamNode);
                if (up == _net.nodes.end())
                    Fail("missing upstream node " + conn.upstreamNode.GetString());
                return HydraOutput(up->second, conn.upstreamOutputName.GetString());
            }
            auto p = node.parameters.find(TfToken(name));
            if (p != node.parameters.end()) {
                const Port *port = def->Input(name);
                return FromVtValue(p->second, port ? port->type : std::string("float"));
            }
            return Default(*def, name);
        };
        Val v = CompileNode(d->second, in, output.empty() ? "out" : output, &node);
        _memo[key] = v;
        return v;
    }

    Val FromVtValue(const VtValue &v, const std::string &type)
    {
        if (v.IsHolding<float>())
            return ConstF(v.UncheckedGet<float>());
        if (v.IsHolding<double>())
            return ConstF(float(v.UncheckedGet<double>()));
        if (v.IsHolding<int>())
            return ConstF(float(v.UncheckedGet<int>()));
        if (v.IsHolding<bool>())
            return ConstF(v.UncheckedGet<bool>() ? 1.f : 0.f);
        if (v.IsHolding<GfVec2f>()) {
            GfVec2f c = v.UncheckedGet<GfVec2f>();
            return Const(V4{c[0], c[1], 0, 0}, 2);
        }
        if (v.IsHolding<GfVec3f>()) {
            GfVec3f c = v.UncheckedGet<GfVec3f>();
            return Const(V4{c[0], c[1], c[2], 0}, 3);
        }
        if (v.IsHolding<GfVec4f>()) {
            GfVec4f c = v.UncheckedGet<GfVec4f>();
            return Const(V4{c[0], c[1], c[2], c[3]}, 4);
        }
        Val s;
        s.isString = true;
        if (v.IsHolding<std::string>())
            s.str = v.UncheckedGet<std::string>();
        else if (v.IsHolding<TfToken>())
            s.str = v.UncheckedGet<TfToken>().GetString();
        else if (v.IsHolding<SdfAssetPath>()) {
            const SdfAssetPath &a = v.UncheckedGet<SdfAssetPath>();
            s.str = a.GetResolvedPath().empty() ? a.GetAssetPath() : a.GetResolvedPath();
        } else if (IsNumeric(type)) {
            Fail("unsupported value type for a " + type + " input");
        }
        return s;
    }

    Val Default(const NodeDef &def, const std::string &name)
    {
        const Port *p = def.Input(name);
        if (!p) {
            Val none;
            return none;
        }
        if (!p->geomprop.empty())
            return GeomProp(p->geomprop, p->type);
        if (!IsNumeric(p->type)) {
            Val s;
            s.isString = true;
            s.str = p->value;
            return s;
        }
        return Const(ParseValue(p->value, p->type), Width(p->type));
    }

    Val GeomProp(const std::string &g, const std::string &type)
    {
        if (g == "UV0")
            return Emit("uv", {}, 2);
        const int space = TfStringEndsWith(g, "world") ? 1 : 0;
        if (TfStringStartsWith(g, "P"))
            return Emit("pos", {space}, 3);
        if (TfStringStartsWith(g, "N"))
            return Emit("nrm", {space}, 3);
        if (TfStringStartsWith(g, "T"))
            return Emit("tan3", {space}, 3);
        if (TfStringStartsWith(g, "B"))
            return Emit("btan", {space}, 3);
        Fail("unsupported default geomprop " + g);
    }

    // ---- nodes -------------------------------------------------------------

    // Compile `def` with inputs from `in`; `hd` is the Hydra node (for
    // metadata such as colorSpace:file), or null inside nodegraphs.
    Val CompileNode(const NodeDef &def, const Resolver &in, const std::string &output,
                    const HdMaterialNode2 *hd)
    {
        auto g = Lib().graphs.find(def.name);
        if (g != Lib().graphs.end() && !Native(def))
            return Expand(def, g->second, in, output);
        return Primitive(def, in, output, hd);
    }

    // Categories implemented directly even when a nodegraph exists.
    static bool Native(const NodeDef &def)
    {
        static const char *native[] = {"noise2d",   "noise3d",   "fractal2d", "fractal3d",
                                       "smoothstep", "convert",  "separate2", "separate3",
                                       "separate4", "distance"};
        for (const char *n : native)
            if (def.category == n)
                return true;
        return false;
    }

    Val Expand(const NodeDef &def, const NodeGraph &graph, const Resolver &outer,
               const std::string &output)
    {
        auto memo = std::make_shared<std::map<std::string, Val>>();
        std::function<Val(const std::string &, const std::string &)> nodeOut;
        nodeOut = [&, memo](const std::string &nodeName, const std::string &out) -> Val {
            const std::string key = nodeName + "." + out;
            auto it = memo->find(key);
            if (it != memo->end())
                return it->second;
            const GNode *gn = graph.Node(nodeName);
            if (!gn)
                Fail("nodegraph of " + def.name + ": no node " + nodeName);
            const NodeDef &nd = Resolve(*gn);
            Resolver in = [&, gn, ndp = &nd](const std::string &name) -> Val {
                for (const GInput &i : gn->inputs) {
                    if (i.name != name)
                        continue;
                    if (!i.nodename.empty())
                        return nodeOut(i.nodename, i.output.empty() ? "out" : i.output);
                    if (!i.interfacename.empty()) {
                        Val v = outer(i.interfacename);
                        if (v.ok())
                            return v;
                        return Default(def, i.interfacename);
                    }
                    if (!IsNumeric(i.type)) {
                        Val s;
                        s.isString = true;
                        s.str = i.value;
                        return s;
                    }
                    return Const(ParseValue(i.value, i.type), Width(i.type));
                }
                return Default(*ndp, name);
            };
            Val v = CompileNode(nd, in, out, nullptr);
            (*memo)[key] = v;
            return v;
        };
        for (const GOutput &o : graph.outputs)
            if (o.name == output) {
                if (o.nodename.empty()) {  // output connected straight to an interface input
                    Fail("nodegraph of " + def.name + ": unsupported output");
                }
                return nodeOut(o.nodename, o.output.empty() ? "out" : o.output);
            }
        Fail("nodegraph of " + def.name + ": no output " + output);
    }

    // The nodedef of a nodegraph node: its category, output type and the
    // types of the inputs it sets.
    const NodeDef &Resolve(const GNode &gn)
    {
        auto c = Lib().byCategory.find(gn.category);
        if (c == Lib().byCategory.end())
            Fail("unknown node category " + gn.category);
        const NodeDef *best = nullptr;
        for (const NodeDef *d : c->second) {
            if (gn.type != "multioutput" &&
                (d->outputs.empty() || d->outputs.front().type != gn.type))
                continue;
            bool match = true;
            for (const GInput &i : gn.inputs) {
                const Port *p = d->Input(i.name);
                if (!p || (!i.type.empty() && p->type != i.type)) {
                    match = false;
                    break;
                }
            }
            if (match) {
                best = d;
                break;
            }
        }
        if (!best)
            Fail("no nodedef for " + gn.category + " (" + gn.type + ")");
        return *best;
    }

    int R(const Val &v, const char *what)
    {
        if (v.reg < 0)
            Fail(std::string("input ") + what + " is not numeric");
        return v.reg;
    }

    std::string Str(const Val &v)
    {
        return v.isString ? v.str : std::string();
    }

    int IntConst(const Val &v, const char *what)
    {
        if (!v.isConst)
            Fail(std::string("input ") + what + " must be constant");
        return int(v.constVal[0]);
    }

    Val Primitive(const NodeDef &def, const Resolver &in, const std::string &output,
                  const HdMaterialNode2 *hd)
    {
        const std::string &c = def.category;
        const std::string outType = def.outputs.empty() ? "float" : def.outputs.front().type;
        if (!IsNumeric(outType) && c != "separate2" && c != "separate3" && c != "separate4")
            Fail(def.name + ": unsupported output type " + outType);
        const int W = Width(outType);
        auto I = [&](const char *name) { return R(in(name), name); };
        auto inW = [&](const char *name) {
            const Port *p = def.Input(name);
            return p ? Width(p->type) : 1;
        };

        static const std::map<std::string, const char *> unary = {
            {"absval", "abs"}, {"sign", "sign"}, {"floor", "floor"}, {"ceil", "ceil"},
            {"round", "round"}, {"fract", "fract"}, {"sin", "sin"}, {"cos", "cos"},
            {"tan", "tan"}, {"asin", "asin"}, {"acos", "acos"}, {"sqrt", "sqrt"},
            {"ln", "ln"}, {"exp", "exp"}};
        static const std::map<std::string, const char *> binary = {
            {"add", "add"}, {"subtract", "sub"}, {"multiply", "mul"}, {"divide", "div"},
            {"modulo", "mod"}, {"power", "pow"}, {"min", "min"}, {"max", "max"}};
        if (auto u = unary.find(c); u != unary.end())
            return Emit(u->second, {I("in")}, W);
        if (auto b = binary.find(c); b != binary.end())
            return Emit(b->second, {I("in1"), I("in2")}, W);
        if (c == "atan2")
            return Emit("atan2", {I("iny"), I("inx")}, W);
        if (c == "constant" || c == "dot")
            return in(c == "constant" ? "value" : "in");
        if (c == "convert")
            return Emit("cvt", {I("in"), inW("in"), W}, W);
        if (c == "clamp")
            return Emit("clamp", {I("in"), I("low"), I("high")}, W);
        if (c == "mix")
            return Emit("mix", {I("fg"), I("bg"), I("mix")}, W);
        if (c == "smoothstep")
            return Emit("smooth", {I("in"), I("low"), I("high")}, W);
        if (c == "remap")
            return Emit("remap", {I("in"), I("inlow"), I("inhigh"), I("outlow"), I("outhigh")},
                        W);
        if (c == "invert")
            return Emit("sub", {I("amount"), I("in")}, W);
        if (c == "ifgreater" || c == "ifgreatereq" || c == "ifequal") {
            const char *op = c == "ifgreater" ? "ifgt" : c == "ifgreatereq" ? "ifge" : "ifeq";
            int a = def.Input("in1") ? I("in1") : ConstF(1).reg;
            int b = def.Input("in2") ? I("in2") : ConstF(0).reg;
            return Emit(op, {I("value1"), I("value2"), a, b}, W);
        }
        if (c == "switch") {
            std::vector<int> args{I("which")};
            for (int k = 1; k <= 10; ++k) {
                std::string n = "in" + std::to_string(k);
                if (def.Input(n))
                    args.push_back(R(in(n), n.c_str()));
            }
            Val r;
            r.reg = NewReg();
            r.width = W;
            code << "switch " << r.reg;
            for (int a : args)
                code << " " << a;
            code << ";";
            return r;
        }
        if (c == "dotproduct")
            return Emit("dot", {I("in1"), I("in2"), inW("in1")}, 1);
        if (c == "distance")
            return Emit("dist", {I("in1"), I("in2"), inW("in1")}, 1);
        if (c == "magnitude")
            return Emit("len", {I("in"), inW("in")}, 1);
        if (c == "normalize")
            return Emit("norm", {I("in"), W}, W);
        if (c == "crossproduct")
            return Emit("cross", {I("in1"), I("in2")}, 3);
        if (c == "extract")
            return Emit("ext", {I("in"), IntConst(in("index"), "index")}, 1);
        if (c == "separate2" || c == "separate3" || c == "separate4") {
            static const char *names[] = {"outx", "outy", "outz", "outw",
                                          "outr", "outg", "outb", "outa"};
            int k = -1;
            for (int i = 0; i < 8; ++i)
                if (output == names[i])
                    k = i % 4;
            if (k < 0)
                Fail(def.name + ": unknown output " + output);
            return Emit("ext", {I("in"), k}, 1);
        }
        if (c == "combine2" || c == "combine3" || c == "combine4") {
            std::vector<int> lanes;
            for (const Port &p : def.inputs) {
                Val v = in(p.name);
                const int w = Width(p.type);
                if (w == 1)
                    lanes.push_back(R(v, p.name.c_str()));
                else
                    for (int k = 0; k < w; ++k)
                        lanes.push_back(Emit("ext", {R(v, p.name.c_str()), k}, 1).reg);
            }
            Val r;
            r.reg = NewReg();
            r.width = W;
            code << "comb " << r.reg;
            for (size_t k = 0; k < lanes.size() && k < 4; ++k)
                code << " " << lanes[k];
            code << ";";
            return r;
        }
        if (c == "rotate2d")
            return Emit("rot2", {I("in"), I("amount")}, 2);
        if (c == "rotate3d")
            return Emit("rot3", {I("in"), I("amount"), I("axis")}, 3);
        if (c == "position" || c == "normal" || c == "tangent" || c == "bitangent") {
            const int space = Str(in("space")) == "world" ? 1 : 0;
            const char *op = c == "position" ? "pos" : c == "normal" ? "nrm"
                             : c == "tangent" ? "tan3" : "btan";
            return Emit(op, {space}, 3);
        }
        if (c == "texcoord")
            return Emit("uv", {}, W);
        if (c == "geompropvalue" || c == "geompropvalueuniform") {
            const std::string g = Str(in("geomprop"));
            if ((g == "st" || g == "uv" || g == "UV0") && W == 2)
                return Emit("uv", {}, 2);
            if (g.empty())
                Fail("geompropvalue without a geomprop");
            return Emit("pv", {PrimvarSlot(g, W), I("default")}, W);
        }
        if (c == "geomcolor")
            return Emit("pv", {PrimvarSlot("displayColor", W), ConstF(1.f).reg}, W);
        if (c == "noise2d" || c == "noise3d") {
            const bool three = c == "noise3d";
            Val n = Emit(three ? "perlin3" : "perlin2", {I(three ? "position" : "texcoord"), W},
                         W);
            Val a = Emit("mul", {n.reg, I("amplitude")}, W);
            return Emit("add", {a.reg, I("pivot")}, W);
        }
        if (c == "fractal2d" || c == "fractal3d") {
            const bool three = c == "fractal3d";
            Val n = Emit(three ? "fractal3" : "fractal2",
                         {I(three ? "position" : "texcoord"), I("octaves"), I("lacunarity"),
                          I("diminish"), W},
                         W);
            return Emit("mul", {n.reg, I("amplitude")}, W);
        }
        if (c == "cellnoise2d" || c == "cellnoise3d")
            return Emit(c == "cellnoise3d" ? "cell3" : "cell2",
                        {I(c == "cellnoise3d" ? "position" : "texcoord"), W}, W);
        if (c == "worleynoise2d" || c == "worleynoise3d") {
            const bool three = c == "worleynoise3d";
            const int style = def.Input("style") ? I("style") : ConstF(0).reg;
            return Emit(three ? "worley3" : "worley2",
                        {I(three ? "position" : "texcoord"), I("jitter"), style, W}, W);
        }
        if (c == "ramplr" || c == "ramptb" || c == "splitlr" || c == "splittb") {
            const int axis = (c == "ramplr" || c == "splitlr") ? 0 : 1;
            Val t = Emit("ext", {I("texcoord"), axis}, 1);
            Val m;
            if (c[0] == 'r')
                m = Emit("clamp", {t.reg, ConstF(0).reg, ConstF(1).reg}, 1);
            else  // step at center
                m = Emit("ifge", {t.reg, I("center"), ConstF(1).reg, ConstF(0).reg}, 1);
            const bool lr = axis == 0;
            return Emit("mix", {I(lr ? "valuer" : "valueb"), I(lr ? "valuel" : "valuet"), m.reg},
                        W);
        }
        if (c == "luminance")
            return Emit("dot", {I("in"), I("lumacoeffs"), 3}, W);
        if (c == "image")
            return Image(def, in, W, hd);
        Fail("unsupported node " + def.name);
    }

    Val Image(const NodeDef &def, const Resolver &in, int W, const HdMaterialNode2 *hd)
    {
        std::string file = Str(in("file"));
        if (TfStringStartsWith(file, "op:"))
            file = _delegate ? _delegate->GetHoudiniImage(file) : std::string();
        if (file.empty() || file.find('<') != std::string::npos)
            Fail("image file missing or a UDIM/token path");
        // Data, unless the file is labeled sRGB.
        std::string encoding = "linear";
        if (hd) {
            auto p = hd->parameters.find(TfToken("colorSpace:file"));
            if (p != hd->parameters.end()) {
                std::string cs = p->second.IsHolding<TfToken>()
                                     ? p->second.UncheckedGet<TfToken>().GetString()
                                     : p->second.IsHolding<std::string>()
                                           ? p->second.UncheckedGet<std::string>()
                                           : std::string();
                cs = TfStringToLower(cs);
                if (cs.find("srgb") != std::string::npos && cs.find("lin") != 0)
                    encoding = "sRGB";
            }
        }
        auto mode = [&](const char *name) {
            const std::string m = Str(in(name));
            return m == "clamp" ? 1 : m == "constant" ? 2 : m == "mirror" ? 3 : 0;
        };
        int index = -1;
        for (size_t i = 0; i + 1 < images.size(); i += 2)
            if (images[i] == file && images[i + 1] == encoding)
                index = int(i / 2);
        if (index < 0) {
            index = int(images.size() / 2);
            images.push_back(file);
            images.push_back(encoding);
        }
        return Emit("img", {index, I_("texcoord", in), I_("default", in), W,
                            mode("uaddressmode"), mode("vaddressmode")},
                    W);
    }
    int I_(const char *name, const Resolver &in) { return R(in(name), name); }

  private:
    const HdMaterialNetwork2 &_net;
    HdPbrtRenderDelegate *_delegate;
    std::map<std::pair<const void *, std::string>, Val> _memo;
    std::map<V4, int> _consts;
};

}  // namespace

bool HdPbrt_CompileDisplacementGraph(const HdMaterialNetwork2 &net, const HdMaterialNode2 &disp,
                                     HdPbrtRenderDelegate *delegate, std::string *program,
                                     std::vector<std::string> *images,
                                     std::vector<std::pair<std::string, int>> *primvars,
                                     std::string *error)
{
    Compiler comp(net, delegate);
    try {
        // pbrlib's displacement node: displacement (float or vector3), scale.
        const std::string type = disp.nodeTypeId.GetString();
        const bool vector = TfStringStartsWith(type, "ND_displacement_vector3");
        if (!vector && !TfStringStartsWith(type, "ND_displacement_float"))
            Fail("unsupported displacement node " + type);
        // The displacement node's own inputs, through the Hydra resolver.
        Compiler::Resolver in = [&](const std::string &name) -> Val {
            auto c = disp.inputConnections.find(TfToken(name));
            if (c != disp.inputConnections.end() && !c->second.empty()) {
                auto up = net.nodes.find(c->second.front().upstreamNode);
                if (up == net.nodes.end())
                    Fail("missing upstream node");
                return comp.HydraOutput(up->second,
                                        c->second.front().upstreamOutputName.GetString());
            }
            auto p = disp.parameters.find(TfToken(name));
            if (p != disp.parameters.end())
                return comp.FromVtValue(p->second, name == "scale" ? "float" : "vector3");
            return name == "scale" ? comp.ConstF(1.f) : comp.ConstF(0.f);
        };
        Val v = in("displacement");
        Val s = in("scale");
        const int w = vector ? 3 : 1;
        Val r = comp.Emit("mul", {comp.R(v, "displacement"), comp.R(s, "scale")}, w);
        comp.code << "out " << r.reg;
    } catch (const CompileError &e) {
        if (error)
            *error = e.what;
        return false;
    }
    *program = comp.code.str();
    *images = comp.images;
    *primvars = comp.primvars;
    return true;
}

PXR_NAMESPACE_CLOSE_SCOPE
