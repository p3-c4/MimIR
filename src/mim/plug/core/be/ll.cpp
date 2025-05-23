#include "mim/plug/core/be/ll.h"

#include <deque>
#include <fstream>
#include <iomanip>
#include <ranges>

#include <mim/plug/clos/clos.h>
#include <mim/plug/math/math.h>
#include <mim/plug/mem/mem.h>

#include "mim/be/emitter.h"
#include "mim/util/print.h"
#include "mim/util/sys.h"

#include "mim/plug/core/core.h"

// Lessons learned:
// * **Always** follow all ops - even if you actually want to ignore one.
//   Otherwise, you might end up with an incorrect schedule.
//   This was the case for an Extract of type Mem.
//   While we want to ignore the value obtained from that, since there is no Mem value in LLVM,
//   we still want to **first** recursively emit code for its operands and **then** ignore the Extract itself.
// * i1 has a different meaning in LLVM then in Mim:
//      * Mim: {0,  1} = i1
//      * LLVM:   {0, -1} = i1
//   This is a problem when, e.g., using an index of type i1 as LLVM thinks like this:
//   getelementptr ..., i1 1 == getelementptr .., i1 -1
using namespace std::string_literals;

namespace mim::ll {

namespace clos = mim::plug::clos;
namespace core = mim::plug::core;
namespace math = mim::plug::math;
namespace mem  = mim::plug::mem;

namespace {
bool is_const(const Def* def) {
    if (def->isa<Bot>()) return true;
    if (def->isa<Lit>()) return true;
    if (auto pack = def->isa_imm<Pack>()) return is_const(pack->shape()) && is_const(pack->body());

    if (auto tuple = def->isa<Tuple>()) {
        auto ops = tuple->ops();
        return std::ranges::all_of(ops, [](auto def) { return is_const(def); });
    }

    return false;
}

const char* math_suffix(const Def* type) {
    if (auto w = math::isa_f(type)) {
        switch (*w) {
            case 32: return "f";
            case 64: return "";
        }
    }
    error("unsupported foating point type '{}'", type);
}

const char* llvm_suffix(const Def* type) {
    if (auto w = math::isa_f(type)) {
        switch (*w) {
            case 16: return ".f16";
            case 32: return ".f32";
            case 64: return ".f64";
        }
    }
    error("unsupported foating point type '{}'", type);
}

// [%mem.M, T] => T
// TODO there may be more instances where we have to deal with this trickery
const Def* isa_mem_sigma_2(const Def* type) {
    if (auto sigma = type->isa<Sigma>())
        if (sigma->num_ops() == 2 && Axm::isa<mem::M>(sigma->op(0))) return sigma->op(1);
    return {};
}
} // namespace

struct BB {
    BB()                    = default;
    BB(const BB&)           = delete;
    BB(BB&& other) noexcept = default;
    BB& operator=(BB other) noexcept { return swap(*this, other), *this; }

    std::deque<std::ostringstream>& head() { return parts[0]; }
    std::deque<std::ostringstream>& body() { return parts[1]; }
    std::deque<std::ostringstream>& tail() { return parts[2]; }

    template<class... Args> std::string assign(std::string_view name, const char* s, Args&&... args) {
        print(print(body().emplace_back(), "{} = ", name), s, std::forward<Args&&>(args)...);
        return std::string(name);
    }

    template<class... Args> void tail(const char* s, Args&&... args) {
        print(tail().emplace_back(), s, std::forward<Args&&>(args)...);
    }

    friend void swap(BB& a, BB& b) noexcept {
        using std::swap;
        swap(a.phis, b.phis);
        swap(a.parts, b.parts);
    }

    DefMap<std::deque<std::pair<std::string, std::string>>> phis;
    std::array<std::deque<std::ostringstream>, 3> parts;
};

class Emitter : public mim::Emitter<std::string, std::string, BB, Emitter> {
public:
    using Super = mim::Emitter<std::string, std::string, BB, Emitter>;

    Emitter(World& world, std::ostream& ostream)
        : Super(world, "llvm_emitter", ostream) {}

    bool is_valid(std::string_view s) { return !s.empty(); }
    void start() override;
    void emit_imported(Lam*);
    void emit_epilogue(Lam*);
    std::string emit_bb(BB&, const Def*);
    std::string prepare();
    void finalize();

    template<class... Args> void declare(const char* s, Args&&... args) {
        std::ostringstream decl;
        print(decl << "declare ", s, std::forward<Args&&>(args)...);
        decls_.emplace(decl.str());
    }

private:
    std::string id(const Def*, bool force_bb = false) const;
    std::string convert(const Def*);
    std::string convert_ret_pi(const Pi*);

    absl::btree_set<std::string> decls_;
    std::ostringstream type_decls_;
    std::ostringstream vars_decls_;
    std::ostringstream func_decls_;
    std::ostringstream func_impls_;
};

/*
 * convert
 */

std::string Emitter::id(const Def* def, bool force_bb /*= false*/) const {
    if (auto global = def->isa<Global>()) return "@" + global->unique_name();

    if (auto lam = def->isa_mut<Lam>(); lam && !force_bb) {
        if (lam->type()->ret_pi()) {
            if (lam->is_external() || !lam->is_set())
                return "@"s + lam->sym().str(); // TODO or use is_internal or sth like that?
            return "@"s + lam->unique_name();
        }
    }

    return "%"s + def->unique_name();
}

std::string Emitter::convert(const Def* type) {
    if (auto i = types_.find(type); i != types_.end()) return i->second;

    assert(!Axm::isa<mem::M>(type));
    std::ostringstream s;
    std::string name;

    if (type->isa<Nat>()) {
        return types_[type] = "i64";
    } else if (auto size = Idx::isa(type)) {
        return types_[type] = "i" + std::to_string(*Idx::size2bitwidth(size));
    } else if (auto w = math::isa_f(type)) {
        switch (*w) {
            case 16: return types_[type] = "half";
            case 32: return types_[type] = "float";
            case 64: return types_[type] = "double";
            default: fe::unreachable();
        }
    } else if (auto ptr = Axm::isa<mem::Ptr>(type)) {
        auto [pointee, addr_space] = ptr->args<2>();
        // TODO addr_space
        print(s, "{}*", convert(pointee));
    } else if (auto arr = type->isa<Arr>()) {
        auto t_elem = convert(arr->body());
        u64 size    = 0;
        if (auto arity = Lit::isa(arr->shape())) size = *arity;
        print(s, "[{} x {}]", size, t_elem);
    } else if (auto pi = type->isa<Pi>()) {
        assert(Pi::isa_returning(pi) && "should never have to convert type of BB");
        print(s, "{} (", convert_ret_pi(pi->ret_pi()));

        if (auto t = isa_mem_sigma_2(pi->dom()))
            s << convert(t);
        else {
            auto doms = pi->doms();
            for (auto sep = ""; auto dom : doms.view().rsubspan(1)) {
                if (Axm::isa<mem::M>(dom)) continue;
                s << sep << convert(dom);
                sep = ", ";
            }
        }
        s << ")*";
    } else if (auto t = isa_mem_sigma_2(type)) {
        return convert(t);
    } else if (auto sigma = type->isa<Sigma>()) {
        if (sigma->isa_mut()) {
            name          = id(sigma);
            types_[sigma] = name;
            print(s, "{} = type", name);
        }

        print(s, "{{");
        for (auto sep = ""; auto t : sigma->ops()) {
            if (Axm::isa<mem::M>(t)) continue;
            s << sep << convert(t);
            sep = ", ";
        }
        print(s, "}}");
    } else {
        fe::unreachable();
    }

    if (name.empty()) return types_[type] = s.str();

    assert(!s.str().empty());
    type_decls_ << s.str() << '\n';
    return types_[type] = name;
}

std::string Emitter::convert_ret_pi(const Pi* pi) {
    auto dom = mem::strip_mem_ty(pi->dom());
    if (dom == world().sigma()) return "void";
    return convert(dom);
}

/*
 * emit
 */

void Emitter::start() {
    Super::start();

    ostream() << type_decls_.str() << '\n';
    for (auto&& decl : decls_) ostream() << decl << '\n';
    ostream() << func_decls_.str() << '\n';
    ostream() << vars_decls_.str() << '\n';
    ostream() << func_impls_.str() << '\n';
}

void Emitter::emit_imported(Lam* lam) {
    // TODO merge with declare method
    print(func_decls_, "declare {} {}(", convert_ret_pi(lam->type()->ret_pi()), id(lam));

    auto doms = lam->doms();
    for (auto sep = ""; auto dom : doms.view().rsubspan(1)) {
        if (Axm::isa<mem::M>(dom)) continue;
        print(func_decls_, "{}{}", sep, convert(dom));
        sep = ", ";
    }

    print(func_decls_, ")\n");
}

std::string Emitter::prepare() {
    print(func_impls_, "define {} {}(", convert_ret_pi(root()->type()->ret_pi()), id(root()));

    auto vars = root()->vars();
    for (auto sep = ""; auto var : vars.view().rsubspan(1)) {
        if (Axm::isa<mem::M>(var->type())) continue;
        auto name    = id(var);
        locals_[var] = name;
        print(func_impls_, "{}{} {}", sep, convert(var->type()), name);
        sep = ", ";
    }

    print(func_impls_, ") {{\n");
    return root()->unique_name();
}

void Emitter::finalize() {
    for (auto& [lam, bb] : lam2bb_) {
        for (const auto& [phi, args] : bb.phis) {
            print(bb.head().emplace_back(), "{} = phi {} ", id(phi), convert(phi->type()));
            for (auto sep = ""; const auto& [arg, pred] : args) {
                print(bb.head().back(), "{}[ {}, {} ]", sep, arg, pred);
                sep = ", ";
            }
        }
    }

    for (auto mut : Scheduler::schedule(nest())) {
        if (auto lam = mut->isa_mut<Lam>()) {
            assert(lam2bb_.contains(lam));
            auto& bb = lam2bb_[lam];
            print(func_impls_, "{}:\n", lam->unique_name());

            ++tab;
            for (const auto& part : bb.parts)
                for (const auto& line : part) tab.print(func_impls_, "{}\n", line.str());
            --tab;
            func_impls_ << std::endl;
        }
    }

    print(func_impls_, "}}\n\n");
}

void Emitter::emit_epilogue(Lam* lam) {
    auto app = lam->body()->as<App>();
    auto& bb = lam2bb_[lam];

    if (app->callee() == root()->ret_var()) { // return
        std::vector<std::string> values;
        std::vector<const Def*> types;

        for (auto arg : app->args()) {
            if (auto val = emit_unsafe(arg); !val.empty()) {
                values.emplace_back(val);
                types.emplace_back(arg->type());
            }
        }

        switch (values.size()) {
            case 0: return bb.tail("ret void");
            case 1: return bb.tail("ret {} {}", convert(types[0]), values[0]);
            default: {
                std::string prev = "undef";
                auto type        = convert(world().sigma(types));
                for (size_t i = 0, n = values.size(); i != n; ++i) {
                    auto v_elem = values[i];
                    auto t_elem = convert(types[i]);
                    auto namei  = "%ret_val." + std::to_string(i);
                    bb.tail("{} = insertvalue {} {}, {} {}, {}", namei, type, prev, t_elem, v_elem, i);
                    prev = namei;
                }

                bb.tail("ret {} {}", type, prev);
            }
        }
    } else if (auto ex = app->callee()->isa<Extract>(); ex && Pi::isa_basicblock(app->callee_type())) {
        // emit_unsafe(app->arg());
        // A call to an extract like constructed for conditionals (else,then)#cond (args)
        // TODO: we can not rely on the structure of the extract (it might be a nested extract)
        for (auto callee_def : ex->tuple()->projs()) {
            // dissect the tuple of lambdas
            auto callee = callee_def->as_mut<Lam>();
            // each callees type should agree with the argument type (should be checked by type checking).
            // Especially, the number of vars should be the number of arguments.
            // TODO: does not hold for complex arguments that are not tuples.
            assert(callee->num_tvars() == app->num_targs());
            size_t n = callee->num_tvars();
            for (size_t i = 0; i != n; ++i) {
                // emits the arguments one by one (TODO: handle together like before)
                if (auto arg = emit_unsafe(app->arg(n, i)); !arg.empty()) {
                    auto phi = callee->var(n, i);
                    assert(!Axm::isa<mem::M>(phi->type()));
                    lam2bb_[callee].phis[phi].emplace_back(arg, id(lam, true));
                    locals_[phi] = id(phi);
                }
            }
        }

        auto c = emit(ex->index());
        if (ex->tuple()->num_projs() == 2) {
            auto [f, t] = ex->tuple()->projs<2>([this](auto def) { return emit(def); });
            return bb.tail("br i1 {}, label {}, label {}", c, t, f);
        } else {
            auto t_c = convert(ex->index()->type());
            bb.tail("switch {} {}, label {} [ ", t_c, c, emit(ex->tuple()->proj(0)));
            for (auto i = 1u; i < ex->tuple()->num_projs(); i++)
                print(bb.tail().back(), "{} {}, label {} ", t_c, std::to_string(i), emit(ex->tuple()->proj(i)));
            print(bb.tail().back(), "]");
        }
    } else if (app->callee()->isa<Bot>()) {
        return bb.tail("ret ; bottom: unreachable");
    } else if (auto callee = Lam::isa_mut_basicblock(app->callee())) { // ordinary jump
        size_t n = callee->num_tvars();
        for (size_t i = 0; i != n; ++i) {
            if (auto arg = emit_unsafe(app->arg(n, i)); !arg.empty()) {
                auto phi = callee->var(n, i);
                assert(!Axm::isa<mem::M>(phi->type()));
                lam2bb_[callee].phis[phi].emplace_back(arg, id(lam, true));
                locals_[phi] = id(phi);
            }
        }
        return bb.tail("br label {}", id(callee));
    } else if (auto longjmp = Axm::isa<clos::longjmp>(app)) {
        declare("void @longjmp(i8*, i32) noreturn");

        auto [mem, jbuf, tag] = app->args<3>();
        emit_unsafe(mem);
        auto v_jb  = emit(jbuf);
        auto v_tag = emit(tag);
        bb.tail("call void @longjmp(i8* {}, i32 {})", v_jb, v_tag);
        return bb.tail("unreachable");
    } else if (Pi::isa_returning(app->callee_type())) { // function call
        auto v_callee = emit(app->callee());

        std::vector<std::string> args;
        auto app_args = app->args();
        for (auto arg : app_args.view().rsubspan(1))
            if (auto v_arg = emit_unsafe(arg); !v_arg.empty()) args.emplace_back(convert(arg->type()) + " " + v_arg);

        if (app->args().back()->isa<Bot>()) {
            // TODO: Perhaps it'd be better to simply η-wrap this prior to the BE...
            assert(convert_ret_pi(app->callee_type()->ret_pi()) == "void");
            bb.tail("call void {}({, })", v_callee, args);
            return bb.tail("unreachable");
        }

        auto ret_lam    = app->args().back()->as_mut<Lam>();
        size_t num_vars = ret_lam->num_vars();
        size_t n        = 0;
        DefVec values(num_vars);
        DefVec types(num_vars);
        for (auto var : ret_lam->vars()) {
            if (Axm::isa<mem::M>(var->type())) continue;
            values[n] = var;
            types[n]  = var->type();
            ++n;
        }

        if (n == 0) {
            bb.tail("call void {}({, })", v_callee, args);
        } else {
            auto name  = "%" + app->unique_name() + "ret";
            auto t_ret = convert_ret_pi(ret_lam->type());
            bb.tail("{} = call {} {}({, })", name, t_ret, v_callee, args);

            for (size_t i = 0, e = ret_lam->num_vars(); i != e; ++i) {
                auto phi = ret_lam->var(i);
                if (Axm::isa<mem::M>(phi->type())) continue;

                auto namei = name;
                if (e > 2) {
                    namei += '.' + std::to_string(i - 1);
                    bb.tail("{} = extractvalue {} {}, {}", namei, t_ret, name, i - 1);
                }
                assert(!Axm::isa<mem::M>(phi->type()));
                lam2bb_[ret_lam].phis[phi].emplace_back(namei, id(lam, true));
                locals_[phi] = id(phi);
            }
        }

        return bb.tail("br label {}", id(ret_lam));
    }
}

std::string Emitter::emit_bb(BB& bb, const Def* def) {
    if (auto lam = def->isa<Lam>()) return id(lam);

    auto name = id(def);
    std::string op;

    auto emit_tuple = [&](const Def* tuple) {
        if (isa_mem_sigma_2(tuple->type())) {
            emit_unsafe(tuple->proj(2, 0));
            return emit(tuple->proj(2, 1));
        }

        if (is_const(tuple)) {
            bool is_array = tuple->type()->isa<Arr>();

            std::string s;
            s += is_array ? "[" : "{";
            auto sep = "";
            for (size_t i = 0, n = tuple->num_projs(); i != n; ++i) {
                auto e = tuple->proj(n, i);
                if (auto v_elem = emit_unsafe(e); !v_elem.empty()) {
                    auto t_elem = convert(e->type());
                    s += sep + t_elem + " " + v_elem;
                    sep = ", ";
                }
            }

            return s += is_array ? "]" : "}";
        }

        std::string prev = "undef";
        auto t           = convert(tuple->type());
        for (size_t src = 0, dst = 0, n = tuple->num_projs(); src != n; ++src) {
            auto e = tuple->proj(n, src);
            if (auto elem = emit_unsafe(e); !elem.empty()) {
                auto elem_t = convert(e->type());
                // TODO: check dst vs src
                auto namei = name + "." + std::to_string(dst);
                prev       = bb.assign(namei, "insertvalue {} {}, {} {}, {}", t, prev, elem_t, elem, dst);
                dst++;
            }
        }
        return prev;
    };

    if (def->isa<Var>()) {
        auto ts = def->type()->projs();
        if (std::ranges::any_of(ts, [](auto t) { return Axm::isa<mem::M>(t); })) return {};
        return emit_tuple(def);
    }

    auto emit_gep_index = [&](const Def* index) {
        auto v_i = emit(index);
        auto t_i = convert(index->type());

        if (auto size = Idx::isa(index->type())) {
            if (auto w = Idx::size2bitwidth(size); w && *w < 64) {
                v_i = bb.assign(name + ".zext",
                                "zext {} {} to i{} ; add one more bit for gep index as it is treated as signed value",
                                t_i, v_i, *w + 1);
                t_i = "i" + std::to_string(*w + 1);
            }
        }

        return std::pair(v_i, t_i);
    };

    if (auto lit = def->isa<Lit>()) {
        if (lit->type()->isa<Nat>() || Idx::isa(lit->type())) {
            return std::to_string(lit->get());
        } else if (auto w = math::isa_f(lit->type())) {
            std::stringstream s;
            u64 hex;

            switch (*w) {
                case 16:
                    s << "0xH" << std::setfill('0') << std::setw(4) << std::right << std::hex << lit->get<u16>();
                    return s.str();
                case 32: {
                    hex = std::bit_cast<u64>(f64(lit->get<f32>()));
                    break;
                }
                case 64: hex = lit->get<u64>(); break;
                default: fe::unreachable();
            }

            s << "0x" << std::setfill('0') << std::setw(16) << std::right << std::hex << hex;
            return s.str();
        }
        fe::unreachable();
    } else if (def->isa<Bot>()) {
        return "undef";
    } else if (auto top = def->isa<Top>()) {
        if (Axm::isa<mem::M>(top->type())) return {};
        // bail out to error below
    } else if (auto tuple = def->isa<Tuple>()) {
        return emit_tuple(tuple);
    } else if (auto pack = def->isa<Pack>()) {
        if (auto lit = Lit::isa(pack->body()); lit && *lit == 0) return "zeroinitializer";
        return emit_tuple(pack);
    } else if (auto extract = def->isa<Extract>()) {
        auto tuple = extract->tuple();
        auto index = extract->index();

        // use select when extracting from 2-element integral tuples
        // literal indices would be normalized away already, if it was possible
        // As they aren't they likely depend on a var, which is implemented as array -> need extractvalue
        if (auto app = extract->type()->isa<App>();
            app && app->callee()->isa<Idx>() && !index->isa<Lit>() && tuple->type()->isa<Arr>()) {
            if (auto arity = tuple->type()->isa_lit_arity(); arity && *arity == 2) {
                auto t                = convert(extract->type());
                auto [elem_a, elem_b] = tuple->projs<2>([&](auto e) { return emit_unsafe(e); });

                return bb.assign(name, "select {} {}, {} {}, {} {}", convert(index->type()), emit(index), t, elem_b, t,
                                 elem_a);
            }
        }

        auto v_tup = emit_unsafe(tuple);

        // this exact location is important: after emitting the tuple -> ordering of mem ops
        // before emitting the index, as it might be a weird value for mem vars.
        if (Axm::isa<mem::M>(extract->type())) return {};

        auto t_tup = convert(tuple->type());
        if (auto li = Lit::isa(index)) {
            if (isa_mem_sigma_2(tuple->type())) return v_tup;
            // Adjust index, if mem is present.
            auto v_i = Axm::isa<mem::M>(tuple->proj(0)->type()) ? std::to_string(*li - 1) : std::to_string(*li);
            return bb.assign(name, "extractvalue {} {}, {}", t_tup, v_tup, v_i);
        }

        auto t_elem     = convert(extract->type());
        auto [v_i, t_i] = emit_gep_index(index);

        print(lam2bb_[root()].body().emplace_front(),
              "{}.alloca = alloca {} ; copy to alloca to emulate extract with store + gep + load", name, t_tup);
        print(bb.body().emplace_back(), "store {} {}, {}* {}.alloca", t_tup, v_tup, t_tup, name);
        print(bb.body().emplace_back(), "{}.gep = getelementptr inbounds {}, {}* {}.alloca, i64 0, {} {}", name, t_tup,
              t_tup, name, t_i, v_i);
        return bb.assign(name, "load {}, {}* {}.gep", t_elem, t_elem, name);
    } else if (auto insert = def->isa<Insert>()) {
        assert(!Axm::isa<mem::M>(insert->tuple()->proj(0)->type()));
        auto t_tup = convert(insert->tuple()->type());
        auto t_val = convert(insert->value()->type());
        auto v_tup = emit(insert->tuple());
        auto v_val = emit(insert->value());
        if (auto idx = Lit::isa(insert->index())) {
            auto v_idx = emit(insert->index());
            return bb.assign(name, "insertvalue {} {}, {} {}, {}", t_tup, v_tup, t_val, v_val, v_idx);
        } else {
            auto t_elem     = convert(insert->value()->type());
            auto [v_i, t_i] = emit_gep_index(insert->index());
            print(lam2bb_[root()].body().emplace_front(),
                  "{}.alloca = alloca {} ; copy to alloca to emulate insert with store + gep + load", name, t_tup);
            print(bb.body().emplace_back(), "store {} {}, {}* {}.alloca", t_tup, v_tup, t_tup, name);
            print(bb.body().emplace_back(), "{}.gep = getelementptr inbounds {}, {}* {}.alloca, i64 0, {} {}", name,
                  t_tup, t_tup, name, t_i, v_i);
            print(bb.body().emplace_back(), "store {} {}, {}* {}.gep", t_val, v_val, t_val, name);
            return bb.assign(name, "load {}, {}* {}.alloca", t_tup, t_tup, name);
        }
    } else if (auto global = def->isa<Global>()) {
        auto v_init                = emit(global->init());
        auto [pointee, addr_space] = Axm::as<mem::Ptr>(global->type())->args<2>();
        print(vars_decls_, "{} = global {} {}\n", name, convert(pointee), v_init);
        return globals_[global] = name;
    } else if (auto nat = Axm::isa<core::nat>(def)) {
        auto [a, b] = nat->args<2>([this](auto def) { return emit(def); });

        switch (nat.id()) {
            case core::nat::add: op = "add"; break;
            case core::nat::sub: op = "sub"; break;
            case core::nat::mul: op = "mul"; break;
        }

        return bb.assign(name, "{} nsw nuw i64 {}, {}", op, a, b);
    } else if (auto ncmp = Axm::isa<core::ncmp>(def)) {
        auto [a, b] = ncmp->args<2>([this](auto def) { return emit(def); });
        op          = "icmp ";

        switch (ncmp.id()) {
                // clang-format off
            case core::ncmp::e:  op += "eq" ; break;
            case core::ncmp::ne: op += "ne" ; break;
            case core::ncmp::g:  op += "ugt"; break;
            case core::ncmp::ge: op += "uge"; break;
            case core::ncmp::l:  op += "ult"; break;
            case core::ncmp::le: op += "ule"; break;
            // clang-format on
            default: fe::unreachable();
        }

        return bb.assign(name, "{} i64 {}, {}", op, a, b);
    } else if (auto idx = Axm::isa<core::idx>(def)) {
        auto x = emit(idx->arg());
        auto s = *Idx::size2bitwidth(Idx::isa(idx->type()));
        auto t = convert(idx->type());
        if (s < 64) return bb.assign(name, "trunc i64 {} to {}", x, t);
        return x;
    } else if (auto bit1 = Axm::isa<core::bit1>(def)) {
        assert(bit1.id() == core::bit1::neg);
        auto x = emit(bit1->arg());
        auto t = convert(bit1->type());
        return bb.assign(name, "xor {} -1, {}", t, x);
    } else if (auto bit2 = Axm::isa<core::bit2>(def)) {
        auto [a, b] = bit2->args<2>([this](auto def) { return emit(def); });
        auto t      = convert(bit2->type());

        auto neg = [&](std::string_view x) { return bb.assign(name + ".neg", "xor {} -1, {}", t, x); };

        switch (bit2.id()) {
                // clang-format off
            case core::bit2::and_: return bb.assign(name, "and {} {}, {}", t, a, b);
            case core::bit2:: or_: return bb.assign(name, "or  {} {}, {}", t, a, b);
            case core::bit2::xor_: return bb.assign(name, "xor {} {}, {}", t, a, b);
            case core::bit2::nand: return neg(bb.assign(name, "and {} {}, {}", t, a, b));
            case core::bit2:: nor: return neg(bb.assign(name, "or  {} {}, {}", t, a, b));
            case core::bit2::nxor: return neg(bb.assign(name, "xor {} {}, {}", t, a, b));
            case core::bit2:: iff: return bb.assign(name, "and {} {}, {}", neg(a), b);
            case core::bit2::niff: return bb.assign(name, "or  {} {}, {}", neg(a), b);
            // clang-format on
            default: fe::unreachable();
        }
    } else if (auto shr = Axm::isa<core::shr>(def)) {
        auto [a, b] = shr->args<2>([this](auto def) { return emit(def); });
        auto t      = convert(shr->type());

        switch (shr.id()) {
            case core::shr::a: op = "ashr"; break;
            case core::shr::l: op = "lshr"; break;
        }

        return bb.assign(name, "{} {} {}, {}", op, t, a, b);
    } else if (auto wrap = Axm::isa<core::wrap>(def)) {
        auto [a, b] = wrap->args<2>([this](auto def) { return emit(def); });
        auto t      = convert(wrap->type());
        auto mode   = Lit::as(wrap->decurry()->arg());

        switch (wrap.id()) {
            case core::wrap::add: op = "add"; break;
            case core::wrap::sub: op = "sub"; break;
            case core::wrap::mul: op = "mul"; break;
            case core::wrap::shl: op = "shl"; break;
        }

        if (mode & core::Mode::nuw) op += " nuw";
        if (mode & core::Mode::nsw) op += " nsw";

        return bb.assign(name, "{} {} {}, {}", op, t, a, b);
    } else if (auto div = Axm::isa<core::div>(def)) {
        auto [m, xy] = div->args<2>();
        auto [x, y]  = xy->projs<2>();
        auto t       = convert(x->type());
        emit_unsafe(m);
        auto a = emit(x);
        auto b = emit(y);

        switch (div.id()) {
            case core::div::sdiv: op = "sdiv"; break;
            case core::div::udiv: op = "udiv"; break;
            case core::div::srem: op = "srem"; break;
            case core::div::urem: op = "urem"; break;
        }

        return bb.assign(name, "{} {} {}, {}", op, t, a, b);
    } else if (auto icmp = Axm::isa<core::icmp>(def)) {
        auto [a, b] = icmp->args<2>([this](auto def) { return emit(def); });
        auto t      = convert(icmp->arg(0)->type());
        op          = "icmp ";

        switch (icmp.id()) {
                // clang-format off
            case core::icmp::e:   op += "eq" ; break;
            case core::icmp::ne:  op += "ne" ; break;
            case core::icmp::sg:  op += "sgt"; break;
            case core::icmp::sge: op += "sge"; break;
            case core::icmp::sl:  op += "slt"; break;
            case core::icmp::sle: op += "sle"; break;
            case core::icmp::ug:  op += "ugt"; break;
            case core::icmp::uge: op += "uge"; break;
            case core::icmp::ul:  op += "ult"; break;
            case core::icmp::ule: op += "ule"; break;
            // clang-format on
            default: fe::unreachable();
        }

        return bb.assign(name, "{} {} {}, {}", op, t, a, b);
    } else if (auto extr = Axm::isa<core::extrema>(def)) {
        auto [x, y]   = extr->args<2>();
        auto t        = convert(x->type());
        auto a        = emit(x);
        auto b        = emit(y);
        std::string f = "llvm.";
        switch (extr.id()) {
            case core::extrema::Sm: f += "smin."; break;
            case core::extrema::SM: f += "smax."; break;
            case core::extrema::sm: f += "umin."; break;
            case core::extrema::sM: f += "umax."; break;
        }
        f += t;
        declare("{} @{}({}, {})", t, f, t, t);
        return bb.assign(name, "tail call {} @{}({} {}, {} {})", t, f, t, a, t, b);
    } else if (auto abs = Axm::isa<core::abs>(def)) {
        auto [m, x]   = abs->args<2>();
        auto t        = convert(x->type());
        auto a        = emit(x);
        std::string f = "llvm.abs." + t;
        declare("{} @{}({}, {})", t, f, t, "i1");
        return bb.assign(name, "tail call {} @{}({} {}, {} {})", t, f, t, a, "i1", "1");
    } else if (auto conv = Axm::isa<core::conv>(def)) {
        auto v_src = emit(conv->arg());
        auto t_src = convert(conv->arg()->type());
        auto t_dst = convert(conv->type());

        nat_t w_src = *Idx::size2bitwidth(Idx::isa(conv->arg()->type()));
        nat_t w_dst = *Idx::size2bitwidth(Idx::isa(conv->type()));

        if (w_src == w_dst) return v_src;

        switch (conv.id()) {
            case core::conv::s: op = w_src < w_dst ? "sext" : "trunc"; break;
            case core::conv::u: op = w_src < w_dst ? "zext" : "trunc"; break;
        }

        return bb.assign(name, "{} {} {} to {}", op, t_src, v_src, t_dst);
    } else if (auto bitcast = Axm::isa<core::bitcast>(def)) {
        auto dst_type_ptr = Axm::isa<mem::Ptr>(bitcast->type());
        auto src_type_ptr = Axm::isa<mem::Ptr>(bitcast->arg()->type());
        auto v_src        = emit(bitcast->arg());
        auto t_src        = convert(bitcast->arg()->type());
        auto t_dst        = convert(bitcast->type());

        if (auto lit = Lit::isa(bitcast->arg()); lit && *lit == 0) return "zeroinitializer";
        // clang-format off
        if (src_type_ptr && dst_type_ptr) return bb.assign(name,  "bitcast {} {} to {}", t_src, v_src, t_dst);
        if (src_type_ptr)                 return bb.assign(name, "ptrtoint {} {} to {}", t_src, v_src, t_dst);
        if (dst_type_ptr)                 return bb.assign(name, "inttoptr {} {} to {}", t_src, v_src, t_dst);
        // clang-format on

        auto size2width = [&](const Def* type) {
            if (type->isa<Nat>()) return 64_n;
            if (auto size = Idx::isa(type)) return *Idx::size2bitwidth(size);
            return 0_n;
        };

        auto src_size = size2width(bitcast->arg()->type());
        auto dst_size = size2width(bitcast->type());

        op = "bitcast";
        if (src_size && dst_size) {
            if (src_size == dst_size) return v_src;
            op = (src_size < dst_size) ? "zext" : "trunc";
        }
        return bb.assign(name, "{} {} {} to {}", op, t_src, v_src, t_dst);
    } else if (auto lea = Axm::isa<mem::lea>(def)) {
        auto [ptr, i]  = lea->args<2>();
        auto pointee   = Axm::as<mem::Ptr>(ptr->type())->arg(0);
        auto v_ptr     = emit(ptr);
        auto t_pointee = convert(pointee);
        auto t_ptr     = convert(ptr->type());
        if (pointee->isa<Sigma>())
            return bb.assign(name, "getelementptr inbounds {}, {} {}, i64 0, i32 {}", t_pointee, t_ptr, v_ptr,
                             Lit::as(i));

        assert(pointee->isa<Arr>());
        auto [v_i, t_i] = emit_gep_index(i);

        return bb.assign(name, "getelementptr inbounds {}, {} {}, i64 0, {} {}", t_pointee, t_ptr, v_ptr, t_i, v_i);
    } else if (auto malloc = Axm::isa<mem::malloc>(def)) {
        declare("i8* @malloc(i64)");

        emit_unsafe(malloc->arg(0));
        auto size  = emit(malloc->arg(1));
        auto ptr_t = convert(Axm::as<mem::Ptr>(def->proj(1)->type()));
        bb.assign(name + "i8", "call i8* @malloc(i64 {})", size);
        return bb.assign(name, "bitcast i8* {} to {}", name + "i8", ptr_t);
    } else if (auto free = Axm::isa<mem::free>(def)) {
        declare("void @free(i8*)");
        emit_unsafe(free->arg(0));
        auto ptr   = emit(free->arg(1));
        auto ptr_t = convert(Axm::as<mem::Ptr>(free->arg(1)->type()));

        bb.assign(name + "i8", "bitcast {} {} to i8*", ptr_t, ptr);
        bb.tail("call void @free(i8* {})", name + "i8");
        return {};
    } else if (auto mslot = Axm::isa<mem::mslot>(def)) {
        emit_unsafe(mslot->arg(0));
        // TODO array with size
        // auto v_size = emit(mslot->arg(1));
        auto [pointee, addr_space] = mslot->decurry()->args<2>();
        print(bb.body().emplace_back(), "{} = alloca {}", name, convert(pointee));
        return name;
    } else if (auto free = Axm::isa<mem::free>(def)) {
        declare("void @free(i8*)");

        emit_unsafe(free->arg(0));
        auto v_ptr = emit(free->arg(1));
        auto t_ptr = convert(Axm::as<mem::Ptr>(free->arg(1)->type()));

        bb.assign(name + "i8", "bitcast {} {} to i8*", t_ptr, v_ptr);
        bb.tail("call void @free(i8* {})", name + "i8");
        return {};
    } else if (auto load = Axm::isa<mem::load>(def)) {
        emit_unsafe(load->arg(0));
        auto v_ptr     = emit(load->arg(1));
        auto t_ptr     = convert(load->arg(1)->type());
        auto t_pointee = convert(Axm::as<mem::Ptr>(load->arg(1)->type())->arg(0));
        return bb.assign(name, "load {}, {} {}", t_pointee, t_ptr, v_ptr);
    } else if (auto store = Axm::isa<mem::store>(def)) {
        emit_unsafe(store->arg(0));
        auto v_ptr = emit(store->arg(1));
        auto v_val = emit(store->arg(2));
        auto t_ptr = convert(store->arg(1)->type());
        auto t_val = convert(store->arg(2)->type());
        print(bb.body().emplace_back(), "store {} {}, {} {}", t_val, v_val, t_ptr, v_ptr);
        return {};
    } else if (auto q = Axm::isa<clos::alloc_jmpbuf>(def)) {
        declare("i64 @jmpbuf_size()");

        emit_unsafe(q->arg());
        auto size = name + ".size";
        bb.assign(size, "call i64 @jmpbuf_size()");
        return bb.assign(name, "alloca i8, i64 {}", size);
    } else if (auto setjmp = Axm::isa<clos::setjmp>(def)) {
        declare("i32 @_setjmp(i8*) returns_twice");

        auto [mem, jmpbuf] = setjmp->arg()->projs<2>();
        emit_unsafe(mem);
        auto v_jb = emit(jmpbuf);
        return bb.assign(name, "call i32 @_setjmp(i8* {})", v_jb);
    } else if (auto arith = Axm::isa<math::arith>(def)) {
        auto [a, b] = arith->args<2>([this](auto def) { return emit(def); });
        auto t      = convert(arith->type());
        auto mode   = Lit::as(arith->decurry()->arg());

        switch (arith.id()) {
            case math::arith::add: op = "fadd"; break;
            case math::arith::sub: op = "fsub"; break;
            case math::arith::mul: op = "fmul"; break;
            case math::arith::div: op = "fdiv"; break;
            case math::arith::rem: op = "frem"; break;
        }

        if (mode == math::Mode::fast)
            op += " fast";
        else {
            // clang-format off
            if (mode & math::Mode::nnan    ) op += " nnan";
            if (mode & math::Mode::ninf    ) op += " ninf";
            if (mode & math::Mode::nsz     ) op += " nsz";
            if (mode & math::Mode::arcp    ) op += " arcp";
            if (mode & math::Mode::contract) op += " contract";
            if (mode & math::Mode::afn     ) op += " afn";
            if (mode & math::Mode::reassoc ) op += " reassoc";
            // clang-format on
        }

        return bb.assign(name, "{} {} {}, {}", op, t, a, b);
    } else if (auto tri = Axm::isa<math::tri>(def)) {
        auto a = emit(tri->arg());
        auto t = convert(tri->type());

        std::string f;

        if (tri.id() == math::tri::sin) {
            f = "llvm.sin"s + llvm_suffix(tri->type());
        } else if (tri.id() == math::tri::cos) {
            f = "llvm.cos"s + llvm_suffix(tri->type());
        } else {
            if (tri.sub() & sub_t(math::tri::a)) f += "a";

            switch (math::tri((tri.id() & 0x3) | Annex::Base<math::tri>)) {
                case math::tri::sin: f += "sin"; break;
                case math::tri::cos: f += "cos"; break;
                case math::tri::tan: f += "tan"; break;
                case math::tri::ahFF: error("this axm is supposed to be unused");
                default: fe::unreachable();
            }

            if (tri.sub() & sub_t(math::tri::h)) f += "h";
            f += math_suffix(tri->type());
        }

        declare("{} @{}({})", t, f, t);
        return bb.assign(name, "tail call {} @{}({} {})", t, f, t, a);
    } else if (auto extrema = Axm::isa<math::extrema>(def)) {
        auto [a, b]   = extrema->args<2>([this](auto def) { return emit(def); });
        auto t        = convert(extrema->type());
        std::string f = "llvm.";
        switch (extrema.id()) {
            case math::extrema::fmin: f += "minnum"; break;
            case math::extrema::fmax: f += "maxnum"; break;
            case math::extrema::ieee754min: f += "minimum"; break;
            case math::extrema::ieee754max: f += "maximum"; break;
        }
        f += llvm_suffix(extrema->type());

        declare("{} @{}({}, {})", t, f, t, t);
        return bb.assign(name, "tail call {} @{}({} {}, {} {})", t, f, t, a, t, b);
    } else if (auto pow = Axm::isa<math::pow>(def)) {
        auto [a, b]   = pow->args<2>([this](auto def) { return emit(def); });
        auto t        = convert(pow->type());
        std::string f = "llvm.pow";
        f += llvm_suffix(pow->type());
        declare("{} @{}({}, {})", t, f, t, t);
        return bb.assign(name, "tail call {} @{}({} {}, {} {})", t, f, t, a, t, b);
    } else if (auto rt = Axm::isa<math::rt>(def)) {
        auto a = emit(rt->arg());
        auto t = convert(rt->type());
        std::string f;
        if (rt.id() == math::rt::sq)
            f = "llvm.sqrt"s + llvm_suffix(rt->type());
        else
            f = "cbrt"s += math_suffix(rt->type());
        declare("{} @{}({})", t, f, t);
        return bb.assign(name, "tail call {} @{}({} {})", t, f, t, a);
    } else if (auto exp = Axm::isa<math::exp>(def)) {
        auto a        = emit(exp->arg());
        auto t        = convert(exp->type());
        std::string f = "llvm.";
        f += (exp.sub() & sub_t(math::exp::log)) ? "log" : "exp";
        f += (exp.sub() & sub_t(math::exp::bin)) ? "2" : (exp.sub() & sub_t(math::exp::dec)) ? "10" : "";
        f += llvm_suffix(exp->type());
        // TODO doesn't work for exp10"
        declare("{} @{}({})", t, f, t);
        return bb.assign(name, "tail call {} @{}({} {})", t, f, t, a);
    } else if (auto er = Axm::isa<math::er>(def)) {
        auto a = emit(er->arg());
        auto t = convert(er->type());
        auto f = er.id() == math::er::f ? "erf"s : "erfc"s;
        f += math_suffix(er->type());
        declare("{} @{}({})", t, f, t);
        return bb.assign(name, "tail call {} @{}({} {})", t, f, t, a);
    } else if (auto gamma = Axm::isa<math::gamma>(def)) {
        auto a        = emit(gamma->arg());
        auto t        = convert(gamma->type());
        std::string f = gamma.id() == math::gamma::t ? "tgamma" : "lgamma";
        f += math_suffix(gamma->type());
        declare("{} @{}({})", t, f, t);
        return bb.assign(name, "tail call {} @{}({} {})", t, f, t, a);
    } else if (auto cmp = Axm::isa<math::cmp>(def)) {
        auto [a, b] = cmp->args<2>([this](auto def) { return emit(def); });
        auto t      = convert(cmp->arg(0)->type());
        op          = "fcmp ";

        switch (cmp.id()) {
                // clang-format off
            case math::cmp::  e: op += "oeq"; break;
            case math::cmp::  l: op += "olt"; break;
            case math::cmp:: le: op += "ole"; break;
            case math::cmp::  g: op += "ogt"; break;
            case math::cmp:: ge: op += "oge"; break;
            case math::cmp:: ne: op += "one"; break;
            case math::cmp::  o: op += "ord"; break;
            case math::cmp::  u: op += "uno"; break;
            case math::cmp:: ue: op += "ueq"; break;
            case math::cmp:: ul: op += "ult"; break;
            case math::cmp::ule: op += "ule"; break;
            case math::cmp:: ug: op += "ugt"; break;
            case math::cmp::uge: op += "uge"; break;
            case math::cmp::une: op += "une"; break;
            // clang-format on
            default: fe::unreachable();
        }

        return bb.assign(name, "{} {} {}, {}", op, t, a, b);
    } else if (auto conv = Axm::isa<math::conv>(def)) {
        auto v_src = emit(conv->arg());
        auto t_src = convert(conv->arg()->type());
        auto t_dst = convert(conv->type());

        auto s_src = math::isa_f(conv->arg()->type());
        auto s_dst = math::isa_f(conv->type());

        switch (conv.id()) {
            case math::conv::f2f: op = s_src < s_dst ? "fpext" : "fptrunc"; break;
            case math::conv::s2f: op = "sitofp"; break;
            case math::conv::u2f: op = "uitofp"; break;
            case math::conv::f2s: op = "fptosi"; break;
            case math::conv::f2u: op = "fptoui"; break;
        }

        return bb.assign(name, "{} {} {} to {}", op, t_src, v_src, t_dst);
    } else if (auto abs = Axm::isa<math::abs>(def)) {
        auto a        = emit(abs->arg());
        auto t        = convert(abs->type());
        std::string f = "llvm.fabs";
        f += llvm_suffix(abs->type());
        declare("{} @{}({})", t, f, t);
        return bb.assign(name, "tail call {} @{}({} {})", t, f, t, a);
    } else if (auto round = Axm::isa<math::round>(def)) {
        auto a        = emit(round->arg());
        auto t        = convert(round->type());
        std::string f = "llvm.";
        switch (round.id()) {
            case math::round::f: f += "floor"; break;
            case math::round::c: f += "ceil"; break;
            case math::round::r: f += "round"; break;
            case math::round::t: f += "trunc"; break;
        }
        f += llvm_suffix(round->type());
        declare("{} @{}({})", t, f, t);
        return bb.assign(name, "tail call {} @{}({} {})", t, f, t, a);
    }
    error("unhandled def in LLVM backend: {} : {}", def, def->type());
}

void emit(World& world, std::ostream& ostream) {
    Emitter emitter(world, ostream);
    emitter.run();
}

int compile(World& world, std::string name) {
#ifdef _WIN32
    auto exe = name + ".exe"s;
#else
    auto exe = name;
#endif
    return compile(world, name + ".ll"s, exe);
}

int compile(World& world, std::string ll, std::string out) {
    std::ofstream ofs(ll);
    emit(world, ofs);
    ofs.close();
    auto cmd = fmt("clang \"{}\" -o \"{}\" -Wno-override-module", ll, out);
    return sys::system(cmd);
}

int compile_and_run(World& world, std::string name, std::string args) {
    if (compile(world, name) == 0) return sys::run(name, args);
    error("compilation failed");
}

} // namespace mim::ll
