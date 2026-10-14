// Phase 14 (bodyfx): bodyfx.vdpk, the sequence boxes of the units' models (pack/BodyFxFormat.h): what their
// blob shadows cover.
#include "BakerCommon.h"

#include "bake/PackWriter.h"
#include "core/Clock.h"
#include "core/Hash.h"
#include "core/Log.h"
#include "db/DbcView.h"
#include "formats/M2.h"
#include "pack/BodyFxFormat.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace vd::baker {
namespace {

// CreatureModelData.dbc (5875): field 2 is the model path, with the old ".mdx" extension.
constexpr u32 kModelPathField = 2;

std::string m2Path(std::string_view path) {
    std::string out(path);
    const usize dot = out.find_last_of('.');
    if (dot != std::string::npos) {
        out.resize(dot);
    }
    return out + ".m2";
}

} // namespace

int bakeBodyFx(const formats::MpqArchiveSet& mpq, const std::filesystem::path& outPath) {
    const Clock::time_point start = Clock::now();
    Result<std::vector<u8>> dbc = mpq.read("DBFilesClient\\CreatureModelData.dbc");
    if (!dbc) {
        VD_LOG_ERROR("CreatureModelData.dbc: %s", dbc.error().message.c_str());
        return 1;
    }
    Result<db::DbcView> table = db::DbcView::parse(dbc.value().data(), dbc.value().size());
    if (!table) {
        VD_LOG_ERROR("CreatureModelData.dbc: %s", table.error().message.c_str());
        return 1;
    }
    // Every model a unit can show (characters too: the races' displays point into the same table), by hash.
    std::map<u64, std::string> models;
    for (u32 r = 0; r < table.value().recordCount(); ++r) {
        const std::string_view path = table.value().getString(r, kModelPathField);
        if (path.size() > 4) {
            const std::string m2 = m2Path(path);
            models.emplace(assetPathHash(m2), m2);
        }
    }
    std::vector<pack::BodyBoundsModel> entries;
    std::vector<pack::BodyBoundsBox> boxes;
    u32 missing = 0, failed = 0;
    for (const auto& [hash, path] : models) {
        if (!mpq.contains(path)) {
            ++missing;
            continue;
        }
        Result<std::vector<u8>> bytes = mpq.read(path);
        Result<formats::M2Model> m2 =
            bytes ? formats::parseM2(bytes.value()) : Result<formats::M2Model>(bytes.error());
        if (!m2) {
            VD_LOG_WARN("%s: %s", path.c_str(), m2.error().message.c_str());
            ++failed;
            continue;
        }
        // Variation 0 of each animation (the others share its size), sorted by animation.
        pack::BodyBoundsModel entry{hash, static_cast<u32>(boxes.size()), 0};
        std::vector<pack::BodyBoundsBox> own;
        for (const formats::M2Sequence& s : m2.value().sequences) {
            if (s.variation != 0 || std::any_of(own.begin(), own.end(), [&](const pack::BodyBoundsBox& b) {
                    return b.animation == s.animationId;
                })) {
                continue;
            }
            pack::BodyBoundsBox box{};
            box.animation = s.animationId;
            std::memcpy(box.min, s.boundsMin.data(), sizeof(box.min));
            std::memcpy(box.max, s.boundsMax.data(), sizeof(box.max));
            own.push_back(box);
        }
        std::sort(own.begin(), own.end(), [](const pack::BodyBoundsBox& a, const pack::BodyBoundsBox& b) {
            return a.animation < b.animation;
        });
        entry.boxCount = static_cast<u32>(own.size());
        boxes.insert(boxes.end(), own.begin(), own.end());
        entries.push_back(entry);
    }
    // std::map iterates by hash: the models are already sorted for the runtime's binary search.
    pack::BodyBoundsHeader header{pack::kBodyBoundsVersion, static_cast<u32>(entries.size()),
                                  static_cast<u32>(boxes.size()), 0};
    std::vector<u8> blob(sizeof(header) + entries.size() * sizeof(pack::BodyBoundsModel) +
                         boxes.size() * sizeof(pack::BodyBoundsBox));
    u8* out = blob.data();
    std::memcpy(out, &header, sizeof(header));
    out += sizeof(header);
    std::memcpy(out, entries.data(), entries.size() * sizeof(pack::BodyBoundsModel));
    out += entries.size() * sizeof(pack::BodyBoundsModel);
    std::memcpy(out, boxes.data(), boxes.size() * sizeof(pack::BodyBoundsBox));

    bake::PackWriter writer;
    if (Result<void> created = writer.create(outPath, kBakerVersion); !created) {
        VD_LOG_ERROR("%s", created.error().message.c_str());
        return 1;
    }
    if (Result<void> added = writer.add(pack::kBodyBoundsAsset, pack::AssetType::BodyBounds, blob); !added) {
        VD_LOG_ERROR("%s", added.error().message.c_str());
        return 1;
    }
    if (Result<void> finished = writer.finish(); !finished) {
        VD_LOG_ERROR("%s", finished.error().message.c_str());
        return 1;
    }
    VD_LOG_INFO("bodyfx: %zu models, %zu sequence boxes (%u missing from the client, %u failed), %.1f KB, "
                "%.1f s -> %s",
                entries.size(), boxes.size(), missing, failed, static_cast<double>(blob.size()) / 1024.0,
                secondsBetween(start, Clock::now()), outPath.string().c_str());
    return failed == 0 ? 0 : 1;
}

} // namespace vd::baker
