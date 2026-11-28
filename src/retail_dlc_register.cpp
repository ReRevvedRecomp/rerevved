#include "game_content.h"
#include "retail_dlc.h"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include <fmt/format.h>

#include <rex/filesystem.h>
#include <rex/filesystem/devices/stfs_container_device.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_manager.h>
#include <rex/system/xcontent.h>

namespace
{

namespace retail_dlc = rerevved::retail_dlc;
namespace rfs        = rex::filesystem;
using rex::X_RESULT;
using rex::X_STATUS;

struct FileCloser
{
    void operator()(rfs::File* file) const
    {
        file->Destroy();
    }
};

using OpenFile = std::unique_ptr<rfs::File, FileCloser>;

OpenFile openEntry(rfs::Entry* entry)
{
    rfs::File* file = nullptr;
    if (!entry || (entry->attributes() & rfs::kFileAttributeDirectory) ||
        entry->Open(rfs::FileAccess::kFileReadData, &file) != X_STATUS_SUCCESS)
    {
        return nullptr;
    }
    return OpenFile(file);
}

bool readAt(rfs::File& file, uint64_t offset, std::span<uint8_t> out)
{
    size_t read = 0;
    return file.ReadSync(out, static_cast<size_t>(offset), &read) == X_STATUS_SUCCESS &&
           read == out.size();
}

// Empty and fills candidate when the package is usable, otherwise why it is refused.
std::string readPackage(const std::filesystem::path& path, retail_dlc::Candidate& candidate)
{
    std::error_code ec;
    const auto      status = std::filesystem::symlink_status(path, ec);
    if (ec || !std::filesystem::is_regular_file(status))
    {
        return "it is not a plain file";
    }
    if (auto error = retail_dlc::CheckFileName(candidate.file); !error.empty())
    {
        return error;
    }
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size > retail_dlc::kMaxPackageBytes)
    {
        return fmt::format("it is unreadable or larger than {} bytes", retail_dlc::kMaxPackageBytes);
    }

    rfs::StfsContainerDevice device("", path);
    device.RequireSingleFileStfs();
    if (!device.Initialize())
    {
        return "it is not a readable single-file STFS package";
    }
    const auto& metadata = device.header().metadata;
    if (metadata.execution_info.title_id != rerevved::kTitleId)
    {
        return fmt::format("it is for title {:08X}", uint32_t(metadata.execution_info.title_id));
    }
    if (metadata.content_type != rex::system::XContentType::kMarketplaceContent)
    {
        return "it is not marketplace content";
    }

    auto token   = openEntry(device.ResolvePath(retail_dlc::kTokenFileName));
    auto archive = openEntry(device.ResolvePath(retail_dlc::kArchiveName));
    if (!token || !archive)
    {
        return "it does not hold name.txt and dlc.fpk";
    }
    const auto tokenSize = token->entry()->size();
    if (tokenSize > retail_dlc::kMaxTokenBytes)
    {
        return "its name.txt is too long";
    }
    std::string text(static_cast<size_t>(tokenSize), '\0');
    if (!readAt(*token, 0, std::span(reinterpret_cast<uint8_t*>(text.data()), text.size())))
    {
        return "its name.txt is unreadable";
    }
    candidate.token = retail_dlc::ParseToken(text);
    if (candidate.token.empty())
    {
        return "its name.txt holds no pack name";
    }

    const rerevved::map_packs::ReadAt read = [&archive](uint64_t offset, std::span<uint8_t> out)
    {
        return readAt(*archive, offset, out);
    };
    return rerevved::map_packs::ParseArchive(archive->entry()->size(), read, retail_dlc::kRetailRules, candidate.contents);
}

} // namespace

void rerevved::retail_dlc::RegisterFolder(const std::filesystem::path& folder)
{
    std::error_code ec;
    if (!std::filesystem::is_directory(folder, ec))
    {
        return;
    }
    std::vector<std::filesystem::path> paths;
    for (std::filesystem::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
    {
        paths.push_back(it->path());
    }
    if (ec)
    {
        REXLOG_ERROR("Retail DLC folder is unreadable: {}", ec.message());
        return;
    }
    std::sort(paths.begin(), paths.end(), [](const auto& left, const auto& right)
              {
                  return left.filename().native() < right.filename().native();
              });

    std::vector<Candidate>             candidates;
    std::vector<std::filesystem::path> accepted;
    for (const auto& path : paths)
    {
        Candidate candidate;
        try
        {
            candidate.file = rex::path_to_utf8(path.filename());
        }
        catch (const std::exception&)
        {
            // An unpaired UTF-16 surrogate has no UTF-8 form, so it cannot name content either.
            REXLOG_ERROR("Retail DLC file refused: its name is not valid Unicode");
            continue;
        }
        if (candidate.file == "README.txt")
        {
            continue;
        }
        if (auto error = readPackage(path, candidate); !error.empty())
        {
            REXLOG_ERROR("Retail DLC '{}' refused: {}", candidate.file, error);
            continue;
        }
        candidates.push_back(std::move(candidate));
        accepted.push_back(path);
    }

    auto*      content   = REX_KERNEL_STATE()->content_manager();
    const auto decisions = SelectPackages(candidates);
    for (size_t index = 0; index < decisions.size(); ++index)
    {
        const auto& decision = decisions[index];
        if (!decision.accepted)
        {
            REXLOG_ERROR("Retail DLC '{}' refused: {}", decision.file, decision.reason);
            continue;
        }
        const X_RESULT result = content->AddTitlePackage(rerevved::kTitleId, decision.file, accepted[index]);
        if (result != X_ERROR_SUCCESS)
        {
            REXLOG_ERROR("Retail DLC '{}' refused: content registration failed ({:08X})", decision.file, result);
            continue;
        }
        REXLOG_INFO("Retail DLC '{}' registered as '{}' with {} scenario(s)", decision.file, candidates[index].token, candidates[index].contents.scenarios.size());
    }
}
