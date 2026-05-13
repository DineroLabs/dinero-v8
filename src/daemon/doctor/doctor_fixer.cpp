// doctor_fixer.cpp - Safe fix execution engine for dinerod doctor
#include "daemon/doctor/doctor_fixer.h"
#include "daemon/doctor/doctor_context.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <iomanip>
#ifdef _WIN32
#include <io.h>
#include <direct.h>
#include <sys/stat.h>   // MinGW provides POSIX-flavored stat()/struct stat + S_IF* constants
#ifndef F_OK
#define F_OK 0
#endif
// MinGW exposes the underscored MSVC names (_access / _unlink / _mkdir);
// the bare POSIX names are forwarded by these macros for cross-platform code.
#define access _access
#define unlink _unlink
#define mkdir(path, mode) _mkdir(path)
// MinGW's <sys/stat.h> already supplies S_IFMT / S_IFDIR; provide S_ISDIR
// as a fallback in case it isn't macro-defined for this build target.
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#endif
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace dinero {
namespace doctor {

// ═══════════════════════════════════════════════════════════════════════════
// Fix dispatch table
// ═══════════════════════════════════════════════════════════════════════════

// Each fix implementation returns {success, message}.
// Implementations MUST be idempotent and safe.

static std::pair<bool, std::string> FixMempoolRemove(const DoctorContext& ctx) {
    std::string path = ctx.DataDir() + "/mempool.dat";
    if (access(path.c_str(), F_OK) != 0) {
        return {true, "mempool.dat already absent"};
    }
    if (unlink(path.c_str()) == 0) {
        return {true, "Removed " + path};
    }
    return {false, "Failed to remove " + path + ": " + std::string(strerror(errno))};
}

static std::pair<bool, std::string> FixCreateMissingDirs(const DoctorContext& ctx) {
    std::vector<std::string> dirs = {
        ctx.DataDir() + "/blockchain",
        ctx.DataDir() + "/blockchain/chaindb",
        ctx.DataDir() + "/wallets",
    };

    int created = 0;
    for (const auto& dir : dirs) {
        struct stat st;
        if (stat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            continue;  // Already exists
        }
        if (mkdir(dir.c_str(), 0700) == 0) {
            created++;
        } else {
            return {false, "Failed to create " + dir + ": " + std::string(strerror(errno))};
        }
    }

    if (created == 0) {
        return {true, "All directories already exist"};
    }
    return {true, "Created " + std::to_string(created) + " missing directory(ies)"};
}

// Dispatch: fix_id -> implementation function
using FixFn = std::pair<bool, std::string>(*)(const DoctorContext&);

struct FixDispatchEntry {
    const char* fix_id;
    FixFn fn;
};

static const FixDispatchEntry kFixDispatch[] = {
    {"mempool.snapshot_sanity.remove", FixMempoolRemove},
    {"storage.permissions.create_dirs", FixCreateMissingDirs},
};

static FixFn FindFixImpl(const std::string& fix_id) {
    for (const auto& entry : kFixDispatch) {
        if (fix_id == entry.fix_id) {
            return entry.fn;
        }
    }
    return nullptr;
}

// ═══════════════════════════════════════════════════════════════════════════
// CollectEligibleFixes
// ═══════════════════════════════════════════════════════════════════════════

std::vector<DoctorFixer::FixCandidate> DoctorFixer::CollectEligibleFixes(
    const DoctorRunResult& run,
    const DoctorConfig& config)
{
    std::vector<FixCandidate> candidates;

    for (const auto& result : run.results) {
        // Only collect fixes from checks that found issues
        if (result.status == CheckStatus::PASS || result.status == CheckStatus::SKIP) {
            continue;
        }

        for (const auto& fix : result.fix_plan) {
            // Filter by eligibility
            bool eligible = false;

            if (config.force_all_fixes) {
                // --yes-i-know-what-im-doing: all fixes eligible
                eligible = true;
            } else if (config.apply_safe_fixes) {
                if (!config.fix_ids.empty()) {
                    // --apply-safe-fixes --fix <id>: named safe fixes only
                    for (const auto& fid : config.fix_ids) {
                        if (fid == fix.id && fix.safe_to_apply) {
                            eligible = true;
                            break;
                        }
                    }
                } else {
                    // --apply-safe-fixes (no --fix): all safe fixes
                    eligible = fix.safe_to_apply;
                }
            }
            // --fix <id> alone (without --apply-safe-fixes) does NOT apply.
            // This prevents accidental mutation without the explicit gate.

            if (!eligible) {
                continue;
            }

            // Must have an implementation
            if (!FindFixImpl(fix.id)) {
                continue;
            }

            candidates.push_back(FixCandidate{fix, result.id});
        }
    }

    return candidates;
}

// ═══════════════════════════════════════════════════════════════════════════
// PreviewFixes
// ═══════════════════════════════════════════════════════════════════════════

void DoctorFixer::PreviewFixes(std::ostream& out,
                                const std::vector<FixCandidate>& candidates)
{
    if (candidates.empty()) {
        out << "No eligible fixes to apply.\n";
        return;
    }

    out << "\n── Fix Plan (" << candidates.size() << " action"
        << (candidates.size() != 1 ? "s" : "") << ") ─────────────────────\n\n";

    for (size_t i = 0; i < candidates.size(); i++) {
        const auto& c = candidates[i];
        out << "  " << (i + 1) << ". " << c.action.id << "\n";
        out << "     Risk: " << to_string(c.action.risk)
            << " | Downtime: " << c.action.expected_downtime
            << " | From: " << c.check_id << "\n";

        if (!c.action.preconditions.empty()) {
            out << "     Preconditions:\n";
            for (const auto& p : c.action.preconditions) {
                out << "       - " << p << "\n";
            }
        }

        for (const auto& step : c.action.steps) {
            out << "     > " << step << "\n";
        }

        if (!c.action.rollback_notes.empty()) {
            out << "     Rollback: " << c.action.rollback_notes << "\n";
        }

        out << "\n";
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// ApplyFixes
// ═══════════════════════════════════════════════════════════════════════════

FixSession DoctorFixer::ApplyFixes(const std::vector<FixCandidate>& candidates,
                                    const DoctorContext& ctx)
{
    FixSession session;

    for (const auto& c : candidates) {
        FixResult result;
        result.fix_id = c.action.id;
        result.check_id = c.check_id;

        FixFn impl = FindFixImpl(c.action.id);
        if (!impl) {
            result.applied = false;
            result.success = false;
            result.message = "No implementation for fix: " + c.action.id;
            session.skipped++;
            session.results.push_back(std::move(result));
            continue;
        }

        result.applied = true;
        auto [ok, msg] = impl(ctx);
        result.success = ok;
        result.message = msg;

        if (ok) {
            session.succeeded++;
        } else {
            session.failed++;
        }
        session.applied++;

        // Track which checks should be re-run after this fix
        bool already_tracked = false;
        for (const auto& id : session.affected_check_ids) {
            if (id == c.check_id) {
                already_tracked = true;
                break;
            }
        }
        if (!already_tracked) {
            session.affected_check_ids.push_back(c.check_id);
        }

        session.results.push_back(std::move(result));
    }

    return session;
}

// ═══════════════════════════════════════════════════════════════════════════
// RenderFixResults
// ═══════════════════════════════════════════════════════════════════════════

void DoctorFixer::RenderFixResults(std::ostream& out, const FixSession& session) {
    if (session.results.empty()) {
        return;
    }

    out << "\n── Fix Results ──────────────────────────────────\n\n";

    for (const auto& r : session.results) {
        const char* tag;
        if (!r.applied) {
            tag = "[SKIP]";
        } else if (r.success) {
            tag = "[ OK ]";
        } else {
            tag = "[FAIL]";
        }

        out << "  " << tag << " " << r.fix_id << "\n";
        out << "         " << r.message << "\n";
    }

    out << "\n  Applied: " << session.applied
        << " | Succeeded: " << session.succeeded
        << " | Failed: " << session.failed
        << " | Skipped: " << session.skipped << "\n";

    if (!session.affected_check_ids.empty()) {
        out << "\n  Re-run these checks to verify fixes:\n";
        for (const auto& id : session.affected_check_ids) {
            out << "    dinerod doctor --checks " << id << "\n";
        }
    }
}

} // namespace doctor
} // namespace dinero
