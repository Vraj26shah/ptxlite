// RegAlloc.cpp - Poletto & Sarkar style linear scan.
//
// 1. Number instructions 0..N-1 in program order.
// 2. For every virtual register build one interval [start, end] covering every
//    point where it is defined, used, or live (from Liveness). Using one hull
//    per register is conservative but always safe, including around loops:
//    a value live around a back edge is live in every block of the loop, so
//    the hull covers the whole loop.
// 3. Walk intervals by increasing start. Expire intervals that ended before
//    the current start (their register becomes free again) and give the
//    current interval the lowest free register of its class.
#include "RegAlloc.h"

#include "CFG.h"

#include <algorithm>
#include <map>

namespace ptxl {

bool allocateRegisters(Function& fn, const RAOptions& opts, DiagEngine& diag, RAResult& result) {
    result = RAResult{};
    const CFG cfg = CFG::build(fn);
    const Liveness live = Liveness::compute(fn, cfg);

    std::map<std::string, LiveInterval> intervals;
    auto touch = [&](const std::string& reg, int pos) {
        auto it = intervals.find(reg);
        if (it == intervals.end()) {
            const RegDecl* decl = fn.findReg(reg);
            LiveInterval li;
            li.reg = reg;
            li.cls = regClassFor(decl ? decl->type : Type::B32);
            li.start = li.end = pos;
            intervals.emplace(reg, li);
        } else {
            it->second.start = std::min(it->second.start, pos);
            it->second.end = std::max(it->second.end, pos);
        }
    };

    std::vector<std::string> uses;
    std::string def;
    for (size_t i = 0; i < fn.body.size(); ++i) {
        const int pos = static_cast<int>(i);
        collectUsesDefs(fn.body[i], uses, def);
        for (const auto& u : uses) touch(u, pos);
        if (!def.empty()) touch(def, pos);
        for (const auto& r : live.liveAfter[i]) touch(r, pos);
    }

    // Sort by start point (ties broken by name so output is deterministic).
    std::vector<LiveInterval*> order;
    for (auto& [name, li] : intervals) order.push_back(&li);
    std::sort(order.begin(), order.end(), [](const LiveInterval* a, const LiveInterval* b) {
        return a->start != b->start ? a->start < b->start : a->reg < b->reg;
    });

    std::array<std::vector<LiveInterval*>, kNumRegClasses> active;
    std::array<std::vector<bool>, kNumRegClasses> busy;

    for (LiveInterval* cur : order) {
        const int c = static_cast<int>(cur->cls);
        // Expire old intervals: anything that ended before we start is free.
        auto& act = active[c];
        for (auto it = act.begin(); it != act.end();) {
            if ((*it)->end < cur->start) {
                busy[c][(*it)->phys] = false;
                it = act.erase(it);
            } else {
                ++it;
            }
        }
        // Lowest free register in this class.
        int reg = 0;
        while (reg < static_cast<int>(busy[c].size()) && busy[c][reg]) ++reg;
        if (reg == static_cast<int>(busy[c].size())) busy[c].push_back(false);
        busy[c][reg] = true;
        cur->phys = reg;
        act.push_back(cur);
        result.physUsed[c] = std::max(result.physUsed[c], static_cast<unsigned>(reg + 1));
    }

    result.virtualRegs = static_cast<unsigned>(intervals.size());
    for (const LiveInterval* li : order) result.intervals.push_back(*li);

    if (result.registerCount() > opts.maxRegisters) {
        diag.error(fn.loc, "function '" + fn.name + "' needs " +
                               std::to_string(result.registerCount()) +
                               " registers but the limit is " + std::to_string(opts.maxRegisters) +
                               " (spilling to local memory is not implemented)");
        return false;
    }

    // Rewrite operands with their physical registers.
    for (auto& inst : fn.body) {
        for (auto& o : inst.ops) {
            if (!o.usesRegister()) continue;
            const LiveInterval& li = intervals.at(o.name);
            o.cls = li.cls;
            o.phys = li.phys;
        }
        if (inst.guard.present()) inst.guard.phys = intervals.at(inst.guard.reg).phys;
    }
    fn.physUsed = result.physUsed;
    fn.allocated = true;
    return true;
}

}  // namespace ptxl
