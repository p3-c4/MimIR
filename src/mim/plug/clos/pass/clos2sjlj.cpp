#include "mim/plug/clos/pass/clos2sjlj.h"

#include <mim/plug/core/core.h>

namespace mim::plug::clos {

namespace {

std::array<const Def*, 3> split(const Def* def) {
    auto new_ops = DefVec(def->num_projs() - 2, nullptr);
    auto& w      = def->world();
    const Def *mem, *env;
    auto j = 0;
    for (size_t i = 0; i < def->num_projs(); i++) {
        auto op = def->proj(i);
        if (op == w.annex<mem::M>() || op->type() == w.annex<mem::M>())
            mem = op;
        else if (i == Clos_Env_Param)
            env = op;
        else
            new_ops[j++] = op;
    }
    assert(mem && env);
    auto remaining = def->is_intro() ? w.tuple(new_ops) : w.sigma(new_ops);
    if (new_ops.size() == 1 && remaining != new_ops[0]) {
        // FIXME: For some reason this is not constant folded away??
        remaining = new_ops[0];
    }
    return {mem, env, remaining};
}

const Def* rebuild(const Def* mem, const Def* env, Defs remaining) {
    auto& w      = mem->world();
    auto new_ops = DefVec(remaining.size() + 2, [&](auto i) -> const Def* {
        static_assert(Clos_Env_Param == 1);
        if (i == 0) return mem;
        if (i == 1) return env;
        return remaining[i - 2];
    });
    return w.tuple(new_ops);
}

} // namespace

void Clos2SJLJ::get_exn_closures(const Def* def, DefSet& visited) {
    if (!def->is_term() || def->isa_mut<Lam>() || visited.contains(def)) return;
    visited.emplace(def);
    if (auto c = isa_clos_lit(def)) {
        auto lam = c.fnc_as_lam();
        if (c.is_basicblock() && !ignore_.contains(lam)) {
            def->world().DLOG("FOUND exn closure: {}", c.fnc_as_lam());
            lam2tag_[c.fnc_as_lam()] = {lam2tag_.size() + 1, c.env()};
        }
        get_exn_closures(c.env(), visited);
    } else {
        for (auto op : def->ops()) get_exn_closures(op, visited);
    }
}

void Clos2SJLJ::get_exn_closures() {
    lam2tag_.clear();
    if (!curr_mut()->is_set() || !Lam::isa_cn(curr_mut())) return;
    auto app = curr_mut()->body()->isa<App>();
    if (!app) return;
    if (auto p = app->callee()->isa<Extract>(); p && isa_clos_type(p->tuple()->type())) {
        auto p2 = p->tuple()->isa<Extract>();
        if (p2 && p2->tuple()->isa<Tuple>()) {
            // branch: Check the closure environments, but be careful not to traverse
            // the closures themselves
            auto branches = p2->tuple()->ops();
            for (auto b : branches) {
                auto c = isa_clos_lit(b);
                if (c) {
                    ignore_.emplace(c.fnc_as_lam());
                    world().DLOG("IGNORE {}", c.fnc_as_lam());
                }
            }
        }
    }
    auto visited = DefSet();
    get_exn_closures(app->arg(), visited);
}

Lam* Clos2SJLJ::get_throw(const Def* dom) {
    auto& w            = world();
    auto [p, inserted] = dom2throw_.emplace(dom, nullptr);
    auto& tlam         = p->second;
    if (inserted || !tlam) {
        tlam                = w.mut_con(clos_sub_env(dom, w.sigma({jb_type(), rb_type(), tag_type()})))->set("throw");
        auto [m0, env, var] = split(tlam->var());
        auto [jbuf, rbuf, tag] = env->projs<3>();
        auto [m1, r]           = mem::op_alloc(var->type(), m0)->projs<2>();
        auto m2                = w.call<mem::store>(Defs{m1, r, var});
        rbuf    = w.call<core::bitcast>(world().call<mem::Ptr0>(world().call<mem::Ptr0>(var->type())), rbuf);
        auto m3 = w.call<mem::store>(Defs{m2, rbuf, r});
        tlam->set(false, w.call<longjmp>(Defs{m3, jbuf, tag}));
        ignore_.emplace(tlam);
    }
    return tlam;
}

Lam* Clos2SJLJ::get_lpad(Lam* lam, const Def* rb) {
    auto& w            = world();
    auto [p, inserted] = lam2lpad_.emplace(w.tuple({lam, rb}), nullptr);
    auto& lpad         = p->second;
    if (inserted || !lpad) {
        auto [_, env_type, dom] = split(lam->dom());
        lpad                    = mem::mut_con(env_type)->set("lpad");
        auto [m, env, __]       = split(lpad->var());
        auto [m1, arg_ptr]      = w.call<mem::load>(Defs{m, rb})->projs<2>();
        arg_ptr                 = w.call<core::bitcast>(world().call<mem::Ptr0>(dom), arg_ptr);
        auto [m2, args]         = w.call<mem::load>(Defs{m1, arg_ptr})->projs<2>();
        auto full_args          = (lam->num_doms() == 3) ? rebuild(m2, env, {args}) : rebuild(m2, env, args->ops());
        lpad->app(false, lam, full_args);
        ignore_.emplace(lpad);
    }
    return lpad;
}

void Clos2SJLJ::enter() {
    auto& w = world();
    get_exn_closures();
    if (lam2tag_.empty()) return;

    {
        auto m0       = mem::mem_var(curr_mut());
        auto [m1, jb] = w.call<clos::alloc_jmpbuf>(m0)->projs<2>();
        auto [m2, rb] = mem::op_slot(void_ptr(), m1)->projs<2>();
        auto new_args = curr_mut()->vars();
        new_args[0]   = m2;
        curr_mut()->reset(curr_mut()->reduce(w.tuple(new_args)));

        cur_jbuf_ = jb;
        cur_rbuf_ = rb;

        // apparently apply() can change the id of the closures, so we have to do it again :(
        get_exn_closures();
    }

    auto body = curr_mut()->body()->as<App>();

    auto branch_type = clos_type(w.cn(w.annex<mem::M>()));
    auto branches    = DefVec(lam2tag_.size() + 1);
    {
        auto env             = w.tuple(body->args().view().subspan(1));
        auto new_callee      = mem::mut_con(env->type())->set("sjlj_wrap");
        auto [m, env_var, _] = split(new_callee->var());
        auto new_args = DefVec(env->num_projs() + 1, [&](size_t i) { return (i == 0) ? m : env_var->proj(i - 1); });
        new_callee->app(false, body->callee(), new_args);
        branches[0] = clos_pack(env, new_callee, branch_type);
    }

    for (auto [exn_lam, p] : lam2tag_) {
        auto [i, env] = p;
        branches[i]   = clos_pack(env, get_lpad(exn_lam, cur_rbuf_), branch_type);
    }

    auto m0 = body->arg(0);
    assert(m0->type() == w.annex<mem::M>());
    auto [m1, tag] = w.call<setjmp>(Defs{m0, cur_jbuf_})->projs<2>();
    tag            = w.call(core::conv::s, branches.size(), tag);
    auto filter    = curr_mut()->filter();
    auto branch    = w.extract(w.tuple(branches), tag);
    curr_mut()->reset({filter, clos_apply(branch, m1)});
}

const Def* Clos2SJLJ::rewrite(const Def* def) {
    if (auto c = isa_clos_lit(def); c && lam2tag_.contains(c.fnc_as_lam())) {
        auto& w     = world();
        auto [i, _] = lam2tag_[c.fnc_as_lam()];
        auto tlam   = get_throw(c.fnc_as_lam()->dom());
        return clos_pack(w.tuple({cur_jbuf_, cur_rbuf_, w.lit_idx(i)}), tlam, c.type());
    }
    return def;
}

} // namespace mim::plug::clos
