#pragma once

#include "consensus/block_index.h"

#include <unordered_map>
#include <vector>

namespace dinero::consensus {

// One view per candidate-selection pass, under its existing graph locks.
// Nothing survives a tip/status/parent change. Historical candidates share
// the active walk; sibling forks share their already inspected branch suffix.
// Hash equality at the same height preserves the existing predicate even
// when import/restart has separate index objects for the same block.
class CandidateAncestryView {
public:
    explicit CandidateAncestryView(const CBlockIndex* active) : active_(active), cursor_(active) {
        if (active) active_by_height_.emplace(active->height, active);
    }

    bool SharesActiveAncestor(const CBlockIndex* candidate) {
        if (!candidate || !active_) return true;
        std::vector<const CBlockIndex*> walked;
        const auto* node = candidate;
        bool shares = false;
        while (node) {
            if (const auto found = branch_results_.find(node); found != branch_results_.end()) {
                shares = found->second;
                break;
            }
            const auto* ancestor = ActiveAtHeight(node->height);
            if (ancestor && ancestor->hash == node->hash) {
                shares = true;
                break;
            }
            walked.push_back(node);
            ++branch_steps_;
            const auto* parent = node->pprev;
            // A valid parent always has lower height. Refuse malformed cycles
            // rather than keeping the ingress lock in an unbounded walk.
            if (parent && parent->height >= node->height) break;
            node = parent;
        }
        for (const auto* visited : walked) branch_results_.emplace(visited, shares);
        return shares;
    }

    std::size_t ActiveWalkSteps() const { return active_steps_; }
    std::size_t BranchWalkSteps() const { return branch_steps_; }

private:
    const CBlockIndex* ActiveAtHeight(uint32_t height) {
        while (cursor_ && cursor_->height > height) {
            const auto* parent = cursor_->pprev;
            ++active_steps_;
            if (parent && parent->height >= cursor_->height) {
                cursor_ = nullptr;
                break;
            }
            cursor_ = parent;
            if (cursor_) active_by_height_.emplace(cursor_->height, cursor_);
        }
        const auto found = active_by_height_.find(height);
        return found == active_by_height_.end() ? nullptr : found->second;
    }

    const CBlockIndex* active_;
    const CBlockIndex* cursor_;
    std::unordered_map<uint32_t, const CBlockIndex*> active_by_height_;
    std::unordered_map<const CBlockIndex*, bool> branch_results_;
    std::size_t active_steps_{0};
    std::size_t branch_steps_{0};
};
} // namespace dinero::consensus
