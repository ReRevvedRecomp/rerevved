#include "gfx_movie_rows.h"

#include "place_badges.h"
#include "retail_ui_art.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <string>

namespace rerevved::gfx_movie_rows
{

namespace
{

constexpr uint16_t kTagEnd           = 0;
constexpr uint16_t kTagShowFrame     = 1;
constexpr uint16_t kTagPlaceObject   = 4;
constexpr uint16_t kTagPlaceObject2  = 26;
constexpr uint16_t kTagDoAction      = 12;
constexpr uint16_t kTagDefineSprite  = 39;
constexpr uint16_t kTagPlaceObject3  = 70;
constexpr uint16_t kTagDefineShape   = 2;
constexpr uint16_t kTagFrameLabel    = 43;
constexpr uint16_t kTagRemoveObject2 = 28;
constexpr uint16_t kTagExternalImage = 1001;

constexpr uint8_t kPlaceHasCharacter = 0x02;

// HasCharacter | HasMatrix | HasColorTransform | HasName, which every row clip uses.
constexpr uint8_t kRowPlaceFlags = 0x2e;

// Retail GFX_WinLoseScreen.gfx: port1..port4 are character 236 at x 330.35 px
// and y 332.6 px, step 73.35 px (twips). port0 is the winner banner.
constexpr uint16_t kWinLoseRowCharacter    = 236;
constexpr uint16_t kWinLoseWinnerCharacter = 266;
constexpr size_t   kWinLoseRetailRows      = 4;
constexpr int32_t  kWinLoseRetailX         = 6607;
constexpr int32_t  kWinLoseRetailFirstY    = 6652;
constexpr int32_t  kWinLoseRetailPitch     = 1467;

// Six rows fit between the winner banner (y 325 px) and the button legend (y 619 px):
// each scales to 0.64 (16.16) about its center and they step 49 px from y 329.9 px.
constexpr int32_t kWinLoseRowScale  = 41943;
constexpr int32_t kWinLoseRowX      = 8786;
constexpr int32_t kWinLoseFirstRowY = 6598;
constexpr int32_t kWinLoseRowPitch  = 980;

// Retail GFX_Histograph.gfx: playerInfoPanel holds playerInfo1..playerInfo6 at
// x -71.15 px, depths 1, 6, ... 26 and y 0, 69.85 px, ... 349.25 px (twips).
constexpr size_t  kHistographRetailRows = 6;
constexpr int32_t kHistographRowX       = -1423;
constexpr int32_t kHistographRowPitch   = 1397;
constexpr int32_t kHistographDepthStep  = 5;

struct Tag
{
    uint16_t code      = 0;
    size_t   start     = 0;
    size_t   bodyStart = 0;
    size_t   bodySize  = 0;

    size_t End() const
    {
        return bodyStart + bodySize;
    }
};

struct Matrix
{
    bool    hasScale   = false;
    int32_t scaleX     = 0;
    int32_t scaleY     = 0;
    int32_t translateX = 0;
    int32_t translateY = 0;
};

struct Place
{
    uint16_t             depth       = 0;
    uint16_t             characterId = 0;
    Matrix               matrix;
    std::vector<uint8_t> colorTransform;
    std::string          name;
};

// A mask hides the depths above its own up to and including clipDepth.
struct ClipMask
{
    uint32_t depth     = 0;
    uint32_t clipDepth = 0;
};

struct PlacedTag
{
    Tag   tag;
    Place place;
    bool  rowForm = false; // fully parsed in the form the row clips use
};

struct Edit
{
    size_t               offset = 0;
    size_t               remove = 0;
    std::vector<uint8_t> insert;
};

uint16_t readU16(std::span<const uint8_t> data, size_t at)
{
    return static_cast<uint16_t>(data[at] | (data[at + 1] << 8));
}

uint32_t readU32(std::span<const uint8_t> data, size_t at)
{
    return uint32_t{ data[at] } | (uint32_t{ data[at + 1] } << 8) |
           (uint32_t{ data[at + 2] } << 16) | (uint32_t{ data[at + 3] } << 24);
}

void appendU16(std::vector<uint8_t>& out, uint16_t value)
{
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8));
}

void appendU32(std::vector<uint8_t>& out, uint32_t value)
{
    appendU16(out, static_cast<uint16_t>(value));
    appendU16(out, static_cast<uint16_t>(value >> 16));
}

struct BitReader
{
    std::span<const uint8_t> data;
    size_t                   bitPosition = 0;

    bool Read(unsigned count, uint32_t& value)
    {
        value = 0;
        if (count > 32 || bitPosition + count > data.size() * 8)
        {
            return false;
        }
        for (unsigned i = 0; i < count; ++i, ++bitPosition)
        {
            const unsigned bit = (data[bitPosition >> 3] >> (7 - (bitPosition & 7))) & 1;
            value              = (value << 1) | bit;
        }
        return true;
    }

    bool ReadSigned(unsigned count, int32_t& value)
    {
        uint32_t raw = 0;
        if (!Read(count, raw))
        {
            return false;
        }
        if (count > 0 && count < 32 && ((raw >> (count - 1)) & 1) != 0)
        {
            raw |= ~uint32_t{ 0 } << count;
        }
        value = static_cast<int32_t>(raw);
        return true;
    }

    bool Skip(unsigned count)
    {
        uint32_t ignored = 0;
        return Read(count, ignored);
    }

    size_t BytesUsed() const
    {
        return (bitPosition + 7) / 8;
    }
};

struct BitWriter
{
    std::vector<uint8_t> bytes;
    size_t               bitCount = 0;

    void Write(unsigned count, uint32_t value)
    {
        for (unsigned i = count; i > 0; --i, ++bitCount)
        {
            if (bitCount % 8 == 0)
            {
                bytes.push_back(0);
            }
            bytes.back() |= static_cast<uint8_t>(((value >> (i - 1)) & 1) << (7 - bitCount % 8));
        }
    }

    void WriteSigned(unsigned count, int32_t value)
    {
        Write(count, static_cast<uint32_t>(value));
    }
};

unsigned signedBits(int32_t value)
{
    unsigned bits = 1;
    while (bits < 31 &&
           (value < -(int64_t{ 1 } << (bits - 1)) || value >= (int64_t{ 1 } << (bits - 1))))
    {
        ++bits;
    }
    return bits;
}

bool parseHeader(std::span<const uint8_t> data, size_t& tagsStart)
{
    if (data.size() < 9 || std::memcmp(data.data(), "GFX", 3) != 0 ||
        readU32(data, 4) != data.size())
    {
        return false;
    }
    const size_t rectBytes = (5 + 4 * size_t{ static_cast<size_t>(data[8] >> 3) } + 7) / 8;
    tagsStart              = 8 + rectBytes + 4;
    return tagsStart <= data.size();
}

// The list must end with a zero length End tag exactly at `end`.
bool parseTags(std::span<const uint8_t> data, size_t begin, size_t end, std::vector<Tag>& tags)
{
    size_t at = begin;
    while (at < end)
    {
        if (end - at < 2)
        {
            return false;
        }
        const uint16_t header = readU16(data, at);
        Tag            tag;
        tag.code      = header >> 6;
        tag.start     = at;
        tag.bodyStart = at + 2;
        tag.bodySize  = header & 63;
        if (tag.bodySize == 63)
        {
            if (end - tag.bodyStart < 4)
            {
                return false;
            }
            tag.bodySize = readU32(data, tag.bodyStart);
            tag.bodyStart += 4;
        }
        if (tag.bodySize > end - tag.bodyStart)
        {
            return false;
        }
        tags.push_back(tag);
        at = tag.End();
        if (tag.code == kTagEnd)
        {
            return tag.bodySize == 0 && at == end;
        }
    }
    return false;
}

bool parseMatrix(std::span<const uint8_t> data, Matrix& matrix, size_t& used)
{
    BitReader reader{ data };
    uint32_t  hasScale  = 0;
    uint32_t  hasRotate = 0;
    uint32_t  bits      = 0;
    if (!reader.Read(1, hasScale) || hasScale != 0 || !reader.Read(1, hasRotate) ||
        hasRotate != 0 || !reader.Read(5, bits) ||
        !reader.ReadSigned(bits, matrix.translateX) ||
        !reader.ReadSigned(bits, matrix.translateY))
    {
        return false;
    }
    used = reader.BytesUsed();
    return true;
}

void appendMatrix(std::vector<uint8_t>& out, const Matrix& matrix)
{
    BitWriter writer;
    writer.Write(1, matrix.hasScale ? 1 : 0);
    if (matrix.hasScale)
    {
        const unsigned bits = std::max(signedBits(matrix.scaleX), signedBits(matrix.scaleY));
        writer.Write(5, bits);
        writer.WriteSigned(bits, matrix.scaleX);
        writer.WriteSigned(bits, matrix.scaleY);
    }
    writer.Write(1, 0);
    const unsigned bits = std::max(signedBits(matrix.translateX), signedBits(matrix.translateY));
    writer.Write(5, bits);
    writer.WriteSigned(bits, matrix.translateX);
    writer.WriteSigned(bits, matrix.translateY);
    out.insert(out.end(), writer.bytes.begin(), writer.bytes.end());
}

bool parsePlace(std::span<const uint8_t> body, Place& place)
{
    if (body.size() < 5 || body[0] != kRowPlaceFlags)
    {
        return false;
    }
    place.depth       = readU16(body, 1);
    place.characterId = readU16(body, 3);
    size_t at         = 5;
    size_t used       = 0;
    if (!parseMatrix(body.subspan(at), place.matrix, used))
    {
        return false;
    }
    at += used;

    BitReader reader{ body.subspan(at) };
    uint32_t  hasAdd = 0;
    uint32_t  hasMul = 0;
    uint32_t  bits   = 0;
    if (!reader.Read(1, hasAdd) || !reader.Read(1, hasMul) || !reader.Read(4, bits))
    {
        return false;
    }
    const size_t transformBytes = (6 + size_t{ bits } * 4 * (hasAdd + hasMul) + 7) / 8;
    if (body.size() - at < transformBytes)
    {
        return false;
    }
    place.colorTransform.assign(body.begin() + static_cast<ptrdiff_t>(at),
                                body.begin() + static_cast<ptrdiff_t>(at + transformBytes));
    at += transformBytes;

    // The name is the rest of the body and ends with its terminator.
    if (body.size() - at < 2 || body.back() != 0)
    {
        return false;
    }
    const size_t nameLength = body.size() - at - 1;
    const auto*  text       = reinterpret_cast<const char*>(body.data() + at);
    if (std::memchr(text, 0, nameLength) != nullptr)
    {
        return false;
    }
    place.name.assign(text, nameLength);
    return true;
}

std::vector<uint8_t> buildPlaceBody(const Place& place)
{
    std::vector<uint8_t> body{ kRowPlaceFlags };
    appendU16(body, place.depth);
    appendU16(body, place.characterId);
    appendMatrix(body, place.matrix);
    body.insert(body.end(), place.colorTransform.begin(), place.colorTransform.end());
    body.insert(body.end(), place.name.begin(), place.name.end());
    body.push_back(0);
    return body;
}

void appendTagHeader(std::vector<uint8_t>& out, uint16_t code, size_t bodySize)
{
    if (bodySize < 63)
    {
        appendU16(out, static_cast<uint16_t>((code << 6) | bodySize));
        return;
    }
    appendU16(out, static_cast<uint16_t>((code << 6) | 63));
    appendU32(out, static_cast<uint32_t>(bodySize));
}

void appendPlaceTag(std::vector<uint8_t>& out, const Place& place)
{
    const std::vector<uint8_t> body = buildPlaceBody(place);
    appendTagHeader(out, kTagPlaceObject2, body.size());
    out.insert(out.end(), body.begin(), body.end());
}

// Reads the name of any PlaceObject2 (every flag combination). The name comes
// before the clip depth and clip actions, so those are never reached.
bool readPlacementName(std::span<const uint8_t> body,
                       std::string&             name,
                       bool&                    hasName,
                       uint32_t&                clipDepth)
{
    constexpr uint8_t kHasClipDepth = 0x40;

    constexpr uint8_t kHasCharacter = 0x02;
    constexpr uint8_t kHasMatrix    = 0x04;
    constexpr uint8_t kHasTransform = 0x08;
    constexpr uint8_t kHasRatio     = 0x10;
    constexpr uint8_t kHasName      = 0x20;

    hasName   = false;
    clipDepth = 0;
    if (body.size() < 3)
    {
        return false;
    }
    const uint8_t flags = body[0];
    size_t        at    = 3;
    if ((flags & kHasCharacter) != 0)
    {
        at += 2;
    }
    if (at > body.size())
    {
        return false;
    }
    if ((flags & kHasMatrix) != 0)
    {
        BitReader reader{ body.subspan(at) };
        uint32_t  hasScale  = 0;
        uint32_t  hasRotate = 0;
        uint32_t  bits      = 0;
        if (!reader.Read(1, hasScale))
        {
            return false;
        }
        if (hasScale != 0 && (!reader.Read(5, bits) || !reader.Skip(bits) ||
                              !reader.Skip(bits)))
        {
            return false;
        }
        if (!reader.Read(1, hasRotate))
        {
            return false;
        }
        if (hasRotate != 0 && (!reader.Read(5, bits) || !reader.Skip(bits) ||
                               !reader.Skip(bits)))
        {
            return false;
        }
        if (!reader.Read(5, bits) || !reader.Skip(bits) || !reader.Skip(bits))
        {
            return false;
        }
        at += reader.BytesUsed();
    }
    if ((flags & kHasTransform) != 0)
    {
        if (at >= body.size())
        {
            return false;
        }
        BitReader reader{ body.subspan(at) };
        uint32_t  hasAdd = 0;
        uint32_t  hasMul = 0;
        uint32_t  bits   = 0;
        if (!reader.Read(1, hasAdd) || !reader.Read(1, hasMul) || !reader.Read(4, bits))
        {
            return false;
        }
        at += (6 + size_t{ bits } * 4 * (hasAdd + hasMul) + 7) / 8;
    }
    if ((flags & kHasRatio) != 0)
    {
        at += 2;
    }
    if (at > body.size())
    {
        return false;
    }
    if ((flags & kHasName) != 0)
    {
        const auto*  text = reinterpret_cast<const char*>(body.data() + at);
        const size_t room = body.size() - at;
        const auto*  end  = static_cast<const char*>(std::memchr(text, 0, room));
        if (!end)
        {
            return false;
        }
        name.assign(text, static_cast<size_t>(end - text));
        hasName = true;
        at += name.size() + 1;
    }
    if ((flags & kHasClipDepth) != 0)
    {
        if (body.size() - at < 2)
        {
            return false;
        }
        clipDepth = readU16(body, at);
    }
    return true;
}

// Gathers a list's PlaceObject2 tags and highest depth. Named placements are kept so the
// patch never duplicates a clip; a tag it cannot account for makes the list unusable.
bool collectPlaces(std::span<const uint8_t> data,
                   const std::vector<Tag>&  tags,
                   std::vector<PlacedTag>&  places,
                   std::vector<ClipMask>&   masks,
                   uint32_t&                maxDepth)
{
    for (const Tag& tag : tags)
    {
        if (tag.code == kTagPlaceObject || tag.code == kTagPlaceObject3)
        {
            return false;
        }
        if (tag.code != kTagPlaceObject2)
        {
            continue;
        }
        const auto  body = data.subspan(tag.bodyStart, tag.bodySize);
        std::string name;
        bool        hasName   = false;
        uint32_t    clipDepth = 0;
        if (!readPlacementName(body, name, hasName, clipDepth))
        {
            return false;
        }
        maxDepth = std::max<uint32_t>(maxDepth, readU16(body, 1));
        if ((body[0] & 0x40) != 0)
        {
            masks.push_back({ readU16(body, 1), clipDepth });
        }
        PlacedTag placed;
        placed.tag     = tag;
        placed.rowForm = parsePlace(body, placed.place);
        if (placed.rowForm || hasName)
        {
            if (!placed.rowForm)
            {
                placed.place.name = name;
            }
            places.push_back(std::move(placed));
        }
    }
    return true;
}

size_t countNamed(const std::vector<PlacedTag>& places, std::string_view name)
{
    return static_cast<size_t>(std::count_if(places.begin(),
                                             places.end(),
                                             [&](const PlacedTag& placed)
                                             {
                                                 return placed.place.name == name;
                                             }));
}

const PlacedTag* findUnique(const std::vector<PlacedTag>& places, std::string_view name)
{
    if (countNamed(places, name) != 1)
    {
        return nullptr;
    }
    const auto it = std::find_if(places.begin(),
                                 places.end(),
                                 [&](const PlacedTag& placed)
                                 {
                                     return placed.place.name == name;
                                 });
    return it->rowForm ? &*it : nullptr;
}

// Offset of the first frame end, past which a placement belongs to a later frame.
size_t firstFrameEnd(const std::vector<Tag>& tags)
{
    for (const Tag& tag : tags)
    {
        if (tag.code == kTagShowFrame)
        {
            return tag.start;
        }
    }
    return SIZE_MAX;
}

bool maskCovers(const std::vector<ClipMask>& masks, uint32_t depth)
{
    return std::any_of(masks.begin(),
                       masks.end(),
                       [&](const ClipMask& mask)
                       {
                           return mask.depth < depth && depth <= mask.clipDepth;
                       });
}

Result fail(std::string_view reason)
{
    return Result{ {}, reason };
}

Result applyEdits(std::span<const uint8_t> source, std::vector<Edit> edits)
{
    std::sort(edits.begin(),
              edits.end(),
              [](const Edit& left, const Edit& right)
              {
                  return left.offset < right.offset;
              });
    Result result;
    result.bytes.reserve(source.size() + 256);
    size_t cursor = 0;
    for (const Edit& edit : edits)
    {
        if (edit.offset < cursor || edit.remove > source.size() - edit.offset)
        {
            return fail("edits overlap");
        }
        result.bytes.insert(result.bytes.end(),
                            source.begin() + static_cast<ptrdiff_t>(cursor),
                            source.begin() + static_cast<ptrdiff_t>(edit.offset));
        result.bytes.insert(result.bytes.end(), edit.insert.begin(), edit.insert.end());
        cursor = edit.offset + edit.remove;
    }
    result.bytes.insert(result.bytes.end(),
                        source.begin() + static_cast<ptrdiff_t>(cursor),
                        source.end());
    if (result.bytes.size() > UINT32_MAX)
    {
        return fail("patched movie is too large");
    }
    std::vector<uint8_t> length;
    appendU32(length, static_cast<uint32_t>(result.bytes.size()));
    std::copy(length.begin(), length.end(), result.bytes.begin() + 4);
    return result;
}

// Retail frames place_1..place_5 each place a sprite filled with Ribbon<n>.dds; places 6
// and 7 copy the place 5 frames, sprite, shape and image tag with new ids and names.
constexpr int         kBadgeRetailPlaces = 5;
constexpr const char* kBadgeImageName    = "Ribbon";

std::string_view frameLabel(std::span<const uint8_t> data, const Tag& tag)
{
    const auto*  text   = reinterpret_cast<const char*>(data.data() + tag.bodyStart);
    const size_t length = strnlen(text, tag.bodySize);
    return length < tag.bodySize ? std::string_view(text, length) : std::string_view();
}

const Tag* findDefinition(std::span<const uint8_t>        data,
                          const std::vector<Tag>&         tags,
                          uint16_t                        id,
                          std::initializer_list<uint16_t> codes)
{
    for (const Tag& tag : tags)
    {
        if (tag.bodySize >= 2 && readU16(data, tag.bodyStart) == id &&
            std::find(codes.begin(), codes.end(), tag.code) != codes.end())
        {
            return &tag;
        }
    }
    return nullptr;
}

// Codes whose body starts with the character id they define.
bool definesCharacter(uint16_t code)
{
    switch (code)
    {
        case 2:
        case 6:
        case 7:
        case 10:
        case 11:
        case 13:
        case 14:
        case 20:
        case 21:
        case 22:
        case 32:
        case 33:
        case 34:
        case 35:
        case 36:
        case 37:
        case 39:
        case 46:
        case 48:
        case 60:
        case 75:
        case 83:
        case 84:
        case 87:
        case 90:
        case 91:
            return true;
        default:
            return code >= 1000 && code < 1100;
    }
}

// The one PlaceObject2 among tags[begin, end), which must name a character.
const Tag* singleCharacterPlace(std::span<const uint8_t> data,
                                const std::vector<Tag>&  tags,
                                size_t                   begin,
                                size_t                   end)
{
    const Tag* found = nullptr;
    for (size_t i = begin; i < end; ++i)
    {
        if (tags[i].code == kTagPlaceObject || tags[i].code == kTagPlaceObject3)
        {
            return nullptr;
        }
        if (tags[i].code == kTagPlaceObject2)
        {
            if (found || tags[i].bodySize < 5 ||
                (data[tags[i].bodyStart] & kPlaceHasCharacter) == 0)
            {
                return nullptr;
            }
            found = &tags[i];
        }
    }
    return found;
}

void setU16(std::vector<uint8_t>& bytes, size_t at, uint16_t value)
{
    bytes[at]     = static_cast<uint8_t>(value);
    bytes[at + 1] = static_cast<uint8_t>(value >> 8);
}

void appendTag(std::vector<uint8_t>& out, uint16_t code, const std::vector<uint8_t>& body)
{
    appendTagHeader(out, code, body.size());
    out.insert(out.end(), body.begin(), body.end());
}

// Adds the badge edits; returns empty on success or when there is no badge clip.
std::string_view addBadgeEdits(std::span<const uint8_t> source,
                               const std::vector<Tag>&  tags,
                               std::vector<Edit>&       edits)
{
    constexpr std::string_view kDiffers = "place badges differ from retail";

    const Tag*       badge = nullptr;
    std::vector<Tag> inner;
    size_t           templateLabel = 0;
    for (const Tag& tag : tags)
    {
        std::vector<Tag> children;
        if (tag.code != kTagDefineSprite || tag.bodySize < 4 ||
            !parseTags(source, tag.bodyStart + 4, tag.End(), children))
        {
            continue;
        }
        std::array<size_t, kBadgeRetailPlaces + 3> labels{};
        for (size_t i = 0; i < children.size(); ++i)
        {
            if (children[i].code != kTagFrameLabel)
            {
                continue;
            }
            const std::string_view label = frameLabel(source, children[i]);
            for (size_t place = 1; place < labels.size(); ++place)
            {
                if (label == "place_" + std::to_string(place))
                {
                    labels[place] = i + 1;
                }
            }
        }
        if (labels[kBadgeRetailPlaces] == 0)
        {
            continue;
        }
        if (badge || labels[kBadgeRetailPlaces + 1] != 0 || labels[kBadgeRetailPlaces + 2] != 0)
        {
            return kDiffers;
        }
        badge         = &tag;
        inner         = std::move(children);
        templateLabel = labels[kBadgeRetailPlaces] - 1;
    }
    if (!badge)
    {
        return {};
    }

    // The place 5 frames run from its label, with the removal of the previous
    // badge before it, to the clip's End tag.
    const size_t endIndex     = inner.size() - 1;
    const size_t segmentStart = templateLabel > 0 && inner[templateLabel - 1].code == kTagRemoveObject2
                                    ? inner[templateLabel - 1].start
                                    : inner[templateLabel].start;
    const Tag*   framePlace   = singleCharacterPlace(source, inner, templateLabel, endIndex);
    size_t       frames       = 0;
    for (size_t i = templateLabel; i < endIndex; ++i)
    {
        frames += inner[i].code == kTagShowFrame ? 1 : 0;
    }
    if (!framePlace || frames == 0)
    {
        return kDiffers;
    }

    const Tag* badgeSprite = findDefinition(
        source, tags, readU16(source, framePlace->bodyStart + 3), { kTagDefineSprite });
    std::vector<Tag> spriteChildren;
    if (!badgeSprite || badgeSprite->bodySize < 4 ||
        !parseTags(source, badgeSprite->bodyStart + 4, badgeSprite->End(), spriteChildren))
    {
        return kDiffers;
    }
    const Tag* shapePlace =
        singleCharacterPlace(source, spriteChildren, 0, spriteChildren.size());
    const Tag* shape = shapePlace ? findDefinition(source,
                                                   tags,
                                                   readU16(source, shapePlace->bodyStart + 3),
                                                   { kTagDefineShape, 22, 32, 83 })
                                  : nullptr;
    if (!shape)
    {
        return kDiffers;
    }

    // The shape's one bitmap fill (types 0x40-0x43) names the place 5 image.
    const Tag* image        = nullptr;
    size_t     fillAt       = 0;
    uint16_t   maxCharacter = 0;
    for (const Tag& tag : tags)
    {
        if (definesCharacter(tag.code) && tag.bodySize >= 2)
        {
            maxCharacter = std::max(maxCharacter, readU16(source, tag.bodyStart));
        }
        if (tag.code != kTagExternalImage || tag.bodySize < 10)
        {
            continue;
        }
        const uint16_t id = readU16(source, tag.bodyStart);
        for (size_t at = shape->bodyStart + 2; at + 3 <= shape->End(); ++at)
        {
            if ((source[at] & 0xFC) == 0x40 && readU16(source, at + 1) == id)
            {
                if (image)
                {
                    return kDiffers;
                }
                image  = &tag;
                fillAt = at + 1 - shape->bodyStart;
            }
        }
    }
    if (!image)
    {
        return kDiffers;
    }
    const std::string retailName = kBadgeImageName + std::to_string(kBadgeRetailPlaces);
    const size_t      nameAt     = image->bodyStart + 8;
    const size_t      fileAt     = nameAt + 1 + source[nameAt];
    auto              text       = [&](size_t at)
    {
        return std::string_view(reinterpret_cast<const char*>(source.data() + at + 1), source[at]);
    };
    if (fileAt >= image->End() || fileAt + 1 + source[fileAt] != image->End() ||
        text(nameAt) != retailName || text(fileAt) != retailName + ".dds")
    {
        return kDiffers;
    }
    constexpr int kAddedPlaces = static_cast<int>(kWinLosePlaceRows) + 1 - kBadgeRetailPlaces;
    if (maxCharacter > UINT16_MAX - 3 * kAddedPlaces)
    {
        return "place badges have no room";
    }

    auto body = [&](const Tag& tag, size_t end)
    {
        return std::vector<uint8_t>(source.begin() + static_cast<ptrdiff_t>(tag.bodyStart),
                                    source.begin() + static_cast<ptrdiff_t>(end));
    };
    std::vector<uint8_t> definitions;
    std::vector<uint8_t> addedFrames;
    uint16_t             nextId = maxCharacter;
    for (int place = kBadgeRetailPlaces + 1; place <= kBadgeRetailPlaces + kAddedPlaces; ++place)
    {
        const uint16_t    imageId  = ++nextId;
        const uint16_t    shapeId  = ++nextId;
        const uint16_t    spriteId = ++nextId;
        const std::string name     = kBadgeImageName + std::to_string(place);
        const std::string file     = name + ".dds";

        auto imageBody = body(*image, nameAt);
        setU16(imageBody, 0, imageId);
        for (const std::string& value : { name, file })
        {
            imageBody.push_back(static_cast<uint8_t>(value.size()));
            imageBody.insert(imageBody.end(), value.begin(), value.end());
        }
        appendTag(definitions, kTagExternalImage, imageBody);

        auto shapeBody = body(*shape, shape->End());
        setU16(shapeBody, 0, shapeId);
        setU16(shapeBody, fillAt, imageId);
        appendTag(definitions, shape->code, shapeBody);

        auto spriteBody = body(*badgeSprite, badgeSprite->End());
        setU16(spriteBody, 0, spriteId);
        setU16(spriteBody, shapePlace->bodyStart + 3 - badgeSprite->bodyStart, shapeId);
        appendTag(definitions, kTagDefineSprite, spriteBody);

        const Tag&           label = inner[templateLabel];
        std::vector<uint8_t> segment(source.begin() + static_cast<ptrdiff_t>(segmentStart),
                                     source.begin() + static_cast<ptrdiff_t>(inner[endIndex].start));
        segment[label.bodyStart - segmentStart + 6] = static_cast<uint8_t>('0' + place);
        setU16(segment, framePlace->bodyStart + 3 - segmentStart, spriteId);
        addedFrames.insert(addedFrames.end(), segment.begin(), segment.end());
    }
    const size_t frameCount = readU16(source, badge->bodyStart + 2) + kAddedPlaces * frames;
    if (frameCount > UINT16_MAX)
    {
        return "place badges have no room";
    }

    // The new definitions precede the badge clip that uses them.
    Edit header;
    header.offset = badge->start;
    header.remove = badge->bodyStart - badge->start;
    header.insert = std::move(definitions);
    appendTagHeader(header.insert, kTagDefineSprite, badge->bodySize + addedFrames.size());
    Edit count;
    count.offset = badge->bodyStart + 2;
    count.remove = 2;
    appendU16(count.insert, static_cast<uint16_t>(frameCount));
    Edit frameEdit;
    frameEdit.offset = inner[endIndex].start;
    frameEdit.insert = std::move(addedFrames);
    edits.push_back(std::move(header));
    edits.push_back(std::move(count));
    edits.push_back(std::move(frameEdit));
    return {};
}

Result patchWinLose(std::span<const uint8_t> source)
{
    size_t tagsStart = 0;
    if (!parseHeader(source, tagsStart))
    {
        return fail("not a GFX movie of its declared size");
    }
    std::vector<Tag> tags;
    if (!parseTags(source, tagsStart, source.size(), tags))
    {
        return fail("tag stream does not end cleanly");
    }
    std::vector<PlacedTag> places;
    std::vector<ClipMask>  masks;
    uint32_t               maxDepth = 0;
    if (!collectPlaces(source, tags, places, masks, maxDepth))
    {
        return fail("unsupported placement tag");
    }

    const PlacedTag* winner = findUnique(places, "port0");
    if (!winner || winner->place.characterId != kWinLoseWinnerCharacter ||
        countNamed(places, "port5") != 0 || countNamed(places, "port6") != 0)
    {
        return fail("win/lose clips differ from retail");
    }
    const size_t                                     frameEnd = firstFrameEnd(tags);
    std::array<const PlacedTag*, kWinLoseRetailRows> rows{};
    for (size_t i = 0; i < rows.size(); ++i)
    {
        rows[i] = findUnique(places, "port" + std::to_string(i + 1));
        if (!rows[i] || rows[i]->place.characterId != kWinLoseRowCharacter ||
            rows[i]->tag.start >= frameEnd || rows[i]->place.matrix.translateX != kWinLoseRetailX ||
            rows[i]->place.matrix.translateY !=
                kWinLoseRetailFirstY + static_cast<int32_t>(i) * kWinLoseRetailPitch)
        {
            return fail("win/lose row clips differ from retail");
        }
    }
    if (maxDepth + 2 > UINT16_MAX || frameEnd == SIZE_MAX)
    {
        return fail("win/lose timeline has no room for new rows");
    }
    if (maskCovers(masks, maxDepth + 1) || maskCovers(masks, maxDepth + 2))
    {
        return fail("a mask covers the new rows");
    }

    std::vector<Edit> edits;

    size_t lastRow = 0;
    for (size_t i = 1; i < rows.size(); ++i)
    {
        if (rows[i]->tag.start > rows[lastRow]->tag.start)
        {
            lastRow = i;
        }
    }
    for (size_t i = 0; i < kWinLosePlaceRows; ++i)
    {
        Place place = rows[i < rows.size() ? i : rows.size() - 1]->place;
        if (i >= rows.size())
        {
            place.depth = static_cast<uint16_t>(maxDepth + 1 + (i - rows.size()));
            place.name  = "port" + std::to_string(i + 1);
        }
        place.matrix.hasScale   = true;
        place.matrix.scaleX     = kWinLoseRowScale;
        place.matrix.scaleY     = kWinLoseRowScale;
        place.matrix.translateX = kWinLoseRowX;
        place.matrix.translateY = kWinLoseFirstRowY + static_cast<int32_t>(i) * kWinLoseRowPitch;

        if (i < rows.size())
        {
            Edit edit;
            edit.offset = rows[i]->tag.start;
            edit.remove = rows[i]->tag.End() - rows[i]->tag.start;
            appendPlaceTag(edit.insert, place);
            edits.push_back(std::move(edit));
        }
        else
        {
            // The new rows follow the last retail row so they stay in frame 1.
            auto it = std::find_if(edits.begin(),
                                   edits.end(),
                                   [&](const Edit& edit)
                                   {
                                       return edit.offset == rows[lastRow]->tag.start;
                                   });
            appendPlaceTag(it->insert, place);
        }
    }
    if (const std::string_view reason = addBadgeEdits(source, tags, edits); !reason.empty())
    {
        return fail(reason);
    }
    return applyEdits(source, std::move(edits));
}

Result patchHistograph(std::span<const uint8_t> source)
{
    size_t tagsStart = 0;
    if (!parseHeader(source, tagsStart))
    {
        return fail("not a GFX movie of its declared size");
    }
    std::vector<Tag> tags;
    if (!parseTags(source, tagsStart, source.size(), tags))
    {
        return fail("tag stream does not end cleanly");
    }
    std::vector<PlacedTag> places;
    std::vector<ClipMask>  outerMasks;
    uint32_t               maxDepth = 0;
    if (!collectPlaces(source, tags, places, outerMasks, maxDepth))
    {
        return fail("unsupported placement tag");
    }
    const PlacedTag* panel = findUnique(places, "playerInfoPanel");
    if (!panel)
    {
        return fail("legend panel differs from retail");
    }

    const Tag* sprite = nullptr;
    for (const Tag& tag : tags)
    {
        if (tag.code == kTagDefineSprite && tag.bodySize >= 4 &&
            readU16(source, tag.bodyStart) == panel->place.characterId)
        {
            if (sprite)
            {
                return fail("legend panel differs from retail");
            }
            sprite = &tag;
        }
    }
    if (!sprite)
    {
        return fail("legend panel differs from retail");
    }

    std::vector<Tag> innerTags;
    if (!parseTags(source, sprite->bodyStart + 4, sprite->End(), innerTags))
    {
        return fail("legend panel tags do not end cleanly");
    }
    std::vector<PlacedTag> rows;
    std::vector<ClipMask>  masks;
    uint32_t               innerDepth = 0;
    if (!collectPlaces(source, innerTags, rows, masks, innerDepth) ||
        countNamed(rows, "playerInfo7") != 0 || countNamed(rows, "playerInfo8") != 0)
    {
        return fail("legend rows differ from retail");
    }
    const size_t     frameEnd = firstFrameEnd(innerTags);
    const PlacedTag* first    = findUnique(rows, "playerInfo1");
    const PlacedTag* last     = nullptr;
    if (!first)
    {
        return fail("legend rows differ from retail");
    }
    const uint16_t firstCharacter = first->place.characterId;
    for (size_t i = 0; i < kHistographRetailRows; ++i)
    {
        const PlacedTag* row = findUnique(rows, "playerInfo" + std::to_string(i + 1));
        if (!row || row->tag.start >= frameEnd ||
            row->place.characterId != firstCharacter ||
            row->place.depth != 1 + static_cast<int32_t>(i) * kHistographDepthStep ||
            row->place.matrix.translateX != kHistographRowX ||
            row->place.matrix.translateY != static_cast<int32_t>(i) * kHistographRowPitch)
        {
            return fail("legend rows differ from retail");
        }
        last = row;
    }
    if (innerDepth + 2 * kHistographDepthStep > UINT16_MAX || frameEnd == SIZE_MAX)
    {
        return fail("legend panel has no room for new rows");
    }
    if (maskCovers(masks, innerDepth + kHistographDepthStep) ||
        maskCovers(masks, innerDepth + 2 * kHistographDepthStep))
    {
        return fail("a mask covers the new rows");
    }

    std::vector<uint8_t> added;
    for (size_t i = kHistographRetailRows; i < kHistographRows; ++i)
    {
        Place place = last->place;
        place.depth = static_cast<uint16_t>(
            innerDepth + (i - kHistographRetailRows + 1) * kHistographDepthStep);
        place.name              = "playerInfo" + std::to_string(i + 1);
        place.matrix.translateY = static_cast<int32_t>(i) * kHistographRowPitch;
        appendPlaceTag(added, place);
    }

    Edit header;
    header.offset = sprite->start;
    header.remove = sprite->bodyStart - sprite->start;
    appendTagHeader(header.insert, kTagDefineSprite, sprite->bodySize + added.size());
    Edit rowsEdit;
    rowsEdit.offset = last->tag.End();
    rowsEdit.insert = std::move(added);
    return applyEdits(source, { std::move(header), std::move(rowsEdit) });
}

bool equalsIgnoreCase(std::string_view left, std::string_view right)
{
    return std::equal(left.begin(),
                      left.end(),
                      right.begin(),
                      right.end(),
                      [](char a, char b)
                      {
                          return std::tolower(static_cast<unsigned char>(a)) ==
                                 std::tolower(static_cast<unsigned char>(b));
                      });
}

constexpr std::array<std::string_view, kPromptMovies> kPromptNames = {
    "gfx_endgamecrawl.gfx",
    "gfx_statscreen.gfx",
    "gfx_waitingoverlay.gfx",
    "hoa_gpdirectory.gfx",
};

// The buttons that have a key image.
constexpr std::array<std::string_view, 10> kPromptLabels = {
    "A",
    "B",
    "X",
    "Y",
    "LB",
    "RB",
    "LT",
    "RT",
    "STARTBUTTON",
    "BACKBUTTON",
};

constexpr std::array<std::string_view, 4> kLegendMarkers = { "A", "B", "X", "Y" };

// DefineShape2, DefineShape3 and DefineShape4
constexpr std::initializer_list<uint16_t> kVectorShapes = { 22, 32, 83 };

constexpr uint16_t kTagImportAssets  = 57;
constexpr uint16_t kTagImportAssets2 = 71;
constexpr uint16_t kTagExporterInfo  = 1000;

// HasCharacter | HasMatrix | HasRatio
constexpr uint8_t  kPromptPlaceFlags = 0x16;
constexpr uint16_t kNoImage          = 0xFFFF;

bool skipMatrix(BitReader& reader)
{
    auto skipPair = [&](uint32_t width)
    {
        return reader.Skip(width) && reader.Skip(width);
    };
    uint32_t present = 0;
    uint32_t bits    = 0;
    for (int part = 0; part < 2; ++part)
    {
        if (!reader.Read(1, present) || (present != 0 && (!reader.Read(5, bits) || !skipPair(bits))))
        {
            return false;
        }
    }
    return reader.Read(5, bits) && skipPair(bits);
}

bool isPromptPlaceBody(std::span<const uint8_t> body)
{
    if (body.size() < 7 || body[0] != kPromptPlaceFlags)
    {
        return false;
    }
    BitReader reader{ body.subspan(5) };
    return skipMatrix(reader) && 5 + reader.BytesUsed() + 2 == body.size();
}

bool importedMaxId(std::span<const uint8_t> data, const Tag& tag, size_t& maxId)
{
    const auto* text = data.data() + tag.bodyStart;
    const auto* nul  = static_cast<const uint8_t*>(std::memchr(text, 0, tag.bodySize));
    if (!nul)
    {
        return false;
    }
    size_t at = static_cast<size_t>(nul - data.data()) + 1 + (tag.code == kTagImportAssets2 ? 2 : 0);
    if (at + 2 > tag.End())
    {
        return false;
    }
    const size_t count = readU16(data, at);
    at += 2;
    for (size_t i = 0; i < count; ++i)
    {
        if (at + 3 > tag.End())
        {
            return false;
        }
        maxId            = std::max<size_t>(maxId, readU16(data, at));
        const auto* name = static_cast<const uint8_t*>(std::memchr(data.data() + at + 2, 0, tag.End() - at - 2));
        if (!name)
        {
            return false;
        }
        at = static_cast<size_t>(name - data.data()) + 1;
    }
    return at == tag.End();
}

struct LegendMovie
{
    std::vector<Tag> tags;
    Tag              sprite;
    std::vector<Tag> inner;
};

bool hasLabel(std::span<const uint8_t> data, const std::vector<Tag>& inner, std::string_view name)
{
    return std::any_of(inner.begin(),
                       inner.end(),
                       [&](const Tag& tag)
                       {
                           return tag.code == kTagFrameLabel && frameLabel(data, tag) == name;
                       });
}

std::string_view parseLegendMovie(std::span<const uint8_t> data, bool donor, LegendMovie& movie)
{
    size_t tagsStart = 0;
    if (!parseHeader(data, tagsStart))
    {
        return donor ? "donor is not a GFX movie of its declared size"
                     : "not a GFX movie of its declared size";
    }
    if (!parseTags(data, tagsStart, data.size(), movie.tags))
    {
        return donor ? "donor tag stream does not end cleanly" : "tag stream does not end cleanly";
    }
    bool found = false;
    for (const Tag& tag : movie.tags)
    {
        std::vector<Tag> children;
        if (tag.code != kTagDefineSprite || tag.bodySize < 4 ||
            !parseTags(data, tag.bodyStart + 4, tag.End(), children) ||
            !std::all_of(kLegendMarkers.begin(), kLegendMarkers.end(), [&](std::string_view marker)
                         {
                             return hasLabel(data, children, marker);
                         }))
        {
            continue;
        }
        if (found)
        {
            return donor ? "donor has several prompt legends" : "several prompt legends";
        }
        found        = true;
        movie.sprite = tag;
        movie.inner  = std::move(children);
    }
    if (!found)
    {
        return donor ? "donor has no prompt legend" : "no prompt legend";
    }
    return {};
}

// Tags [begin, end) of the labelled frame; end is its ShowFrame.
bool labelFrame(std::span<const uint8_t> data,
                const LegendMovie&       movie,
                std::string_view         label,
                size_t&                  begin,
                size_t&                  end)
{
    const std::vector<Tag>& inner = movie.inner;
    size_t                  index = SIZE_MAX;
    for (size_t i = 0; i < inner.size(); ++i)
    {
        if (inner[i].code == kTagFrameLabel && frameLabel(data, inner[i]) == label)
        {
            if (index != SIZE_MAX)
            {
                return false;
            }
            index = i;
        }
    }
    if (index == SIZE_MAX)
    {
        return false;
    }
    begin = 0;
    for (size_t i = 0; i < index; ++i)
    {
        begin = inner[i].code == kTagShowFrame ? i + 1 : begin;
    }
    for (end = index; end < inner.size(); ++end)
    {
        if (inner[end].code == kTagShowFrame)
        {
            return true;
        }
    }
    return false;
}

struct SpriteShape
{
    Tag  sprite;
    Tag  shapePlace;
    Tag  shape;
    bool plain = false; // holds nothing besides the placement
};

std::optional<SpriteShape> findSpriteShape(std::span<const uint8_t>        data,
                                           const std::vector<Tag>&         tags,
                                           uint16_t                        spriteId,
                                           std::initializer_list<uint16_t> shapeCodes)
{
    const Tag*       sprite = findDefinition(data, tags, spriteId, { kTagDefineSprite });
    std::vector<Tag> children;
    if (!sprite || sprite->bodySize < 4 ||
        !parseTags(data, sprite->bodyStart + 4, sprite->End(), children))
    {
        return std::nullopt;
    }
    const Tag* place = singleCharacterPlace(data, children, 0, children.size());
    const Tag* shape =
        place ? findDefinition(data, tags, readU16(data, place->bodyStart + 3), shapeCodes) : nullptr;
    if (!shape)
    {
        return std::nullopt;
    }
    // Retail LT sprites also carry an empty DoAction, which does nothing.
    const bool plain = std::all_of(children.begin(),
                                   children.end(),
                                   [&](const Tag& child)
                                   {
                                       return child.code == kTagPlaceObject2 || child.code == kTagShowFrame ||
                                              child.code == kTagEnd ||
                                              (child.code == kTagDoAction && child.bodySize == 1 &&
                                               data[child.bodyStart] == 0);
                                   });
    return SpriteShape{ *sprite, *place, *shape, plain };
}

// False when any fill is not a bitmap. Retail shapes lead with a bitmap that names no image.
bool bitmapFillIds(std::span<const uint8_t> data, const Tag& shape, std::vector<size_t>& ids)
{
    if (shape.bodySize < 3)
    {
        return false;
    }
    size_t at = shape.bodyStart + 2;
    at += (5 + 4 * size_t{ static_cast<size_t>(data[at] >> 3) } + 7) / 8;
    if (at >= shape.End())
    {
        return false;
    }
    const size_t count = data[at++];
    for (size_t i = 0; i < count; ++i)
    {
        if (at + 3 > shape.End() || (data[at] & 0xFC) != 0x40)
        {
            return false;
        }
        ids.push_back(at + 1);
        at += 3;

        BitReader reader{ data.subspan(at, shape.End() - at) };
        if (!skipMatrix(reader))
        {
            return false;
        }
        at += reader.BytesUsed();
    }
    return true;
}

std::string_view imageFileName(std::span<const uint8_t> data, const Tag& image)
{
    if (image.bodySize < 10)
    {
        return {};
    }
    const size_t nameAt = image.bodyStart + 8;
    const size_t fileAt = nameAt + 1 + data[nameAt];
    if (fileAt >= image.End() || fileAt + 1 + data[fileAt] != image.End())
    {
        return {};
    }
    return std::string_view(reinterpret_cast<const char*>(data.data() + fileAt + 1), data[fileAt]);
}

struct DonorPrompt
{
    Tag    place;
    Tag    sprite;
    Tag    shapePlace;
    Tag    shape;
    Tag    image;
    size_t fillAt = 0;
};

std::optional<DonorPrompt> findDonorPrompt(std::span<const uint8_t> data,
                                           const LegendMovie&       movie,
                                           std::string_view         label)
{
    size_t begin = 0;
    size_t end   = 0;
    if (!labelFrame(data, movie, label, begin, end))
    {
        return std::nullopt;
    }
    const Tag* place = singleCharacterPlace(data, movie.inner, begin, end);
    if (!place || !isPromptPlaceBody(data.subspan(place->bodyStart, place->bodySize)))
    {
        return std::nullopt;
    }
    const auto found = findSpriteShape(
        data, movie.tags, readU16(data, place->bodyStart + 3), { kTagDefineShape });
    std::vector<size_t> fills;
    if (!found || !found->plain || !bitmapFillIds(data, found->shape, fills))
    {
        return std::nullopt;
    }

    // Each fill is the blank bitmap or the one image, which must be named after the button.
    const Tag* image  = nullptr;
    size_t     fillAt = 0;
    for (const size_t at : fills)
    {
        const uint16_t id = readU16(data, at);
        if (id == kNoImage)
        {
            continue;
        }
        const Tag* candidate = findDefinition(data, movie.tags, id, { kTagExternalImage });
        if (!candidate || image)
        {
            return std::nullopt;
        }
        image  = candidate;
        fillAt = at;
    }
    if (!image || !equalsIgnoreCase(imageFileName(data, *image), std::string(label) + ".dds"))
    {
        return std::nullopt;
    }
    return DonorPrompt{ *place, found->sprite, found->shapePlace, found->shape, *image, fillAt };
}

const Tag* findTargetPlace(std::span<const uint8_t> data, const LegendMovie& movie, std::string_view label)
{
    size_t begin = 0;
    size_t end   = 0;
    if (!labelFrame(data, movie, label, begin, end))
    {
        return nullptr;
    }
    const Tag* place = singleCharacterPlace(data, movie.inner, begin, end);
    if (!place || !findSpriteShape(data,
                                   movie.tags,
                                   readU16(data, place->bodyStart + 3),
                                   kVectorShapes))
    {
        return nullptr;
    }
    return place;
}

std::vector<uint8_t> tagBody(std::span<const uint8_t> data, const Tag& tag)
{
    return std::vector<uint8_t>(data.begin() + static_cast<ptrdiff_t>(tag.bodyStart),
                                data.begin() + static_cast<ptrdiff_t>(tag.End()));
}

} // namespace

Result PatchPromptMovie(std::span<const uint8_t> target, std::span<const uint8_t> donor)
{
    constexpr size_t kIdsPerPrompt = 3;

    LegendMovie targetMovie;
    if (const auto reason = parseLegendMovie(target, false, targetMovie); !reason.empty())
    {
        return fail(reason);
    }
    LegendMovie donorMovie;
    if (const auto reason = parseLegendMovie(donor, true, donorMovie); !reason.empty())
    {
        return fail(reason);
    }

    // New ids clear every character the target defines or imports.
    size_t maxCharacter = 0;
    for (const Tag& tag : targetMovie.tags)
    {
        if (tag.code == kTagImportAssets || tag.code == kTagImportAssets2)
        {
            if (!importedMaxId(target, tag, maxCharacter))
            {
                return fail("prompt movie has an unreadable import tag");
            }
        }
        else if (tag.code != kTagExporterInfo && definesCharacter(tag.code) && tag.bodySize >= 2)
        {
            maxCharacter = std::max<size_t>(maxCharacter, readU16(target, tag.bodyStart));
        }
    }
    if (maxCharacter + kIdsPerPrompt * kPromptLabels.size() > UINT16_MAX)
    {
        return fail("prompt movie has no room for new characters");
    }

    std::vector<uint8_t> definitions;
    std::vector<Edit>    edits;
    size_t               spriteSize = targetMovie.sprite.bodySize;
    auto                 nextId     = static_cast<uint16_t>(maxCharacter);
    for (const std::string_view label : kPromptLabels)
    {
        // A button missing from either movie is skipped; one present in both must match.
        if (!hasLabel(target, targetMovie.inner, label) || !hasLabel(donor, donorMovie.inner, label))
        {
            continue;
        }
        const Tag* place = findTargetPlace(target, targetMovie, label);
        if (!place)
        {
            return fail("a target prompt frame is not a vector prompt");
        }
        const std::optional<DonorPrompt> from = findDonorPrompt(donor, donorMovie, label);
        if (!from)
        {
            return fail("a donor prompt frame is not an image prompt");
        }
        const uint16_t imageId  = ++nextId;
        const uint16_t shapeId  = ++nextId;
        const uint16_t spriteId = ++nextId;

        auto imageBody = tagBody(donor, from->image);
        setU16(imageBody, 0, imageId);
        appendTag(definitions, kTagExternalImage, imageBody);

        auto shapeBody = tagBody(donor, from->shape);
        setU16(shapeBody, 0, shapeId);
        setU16(shapeBody, from->fillAt - from->shape.bodyStart, imageId);
        appendTag(definitions, from->shape.code, shapeBody);

        auto spriteBody = tagBody(donor, from->sprite);
        setU16(spriteBody, 0, spriteId);
        setU16(spriteBody, from->shapePlace.bodyStart + 3 - from->sprite.bodyStart, shapeId);
        appendTag(definitions, kTagDefineSprite, spriteBody);

        // The donor placement keeps its matrix; the target's depth keeps its RemoveObject2 valid.
        auto placeBody = tagBody(donor, from->place);
        setU16(placeBody, 1, readU16(target, place->bodyStart + 1));
        setU16(placeBody, 3, spriteId);
        Edit edit;
        edit.offset = place->start;
        edit.remove = place->End() - place->start;
        appendTag(edit.insert, kTagPlaceObject2, placeBody);
        spriteSize = spriteSize - edit.remove + edit.insert.size();
        edits.push_back(std::move(edit));
    }
    if (edits.empty())
    {
        return fail("target and donor share no prompt button");
    }

    // Definitions must precede the sprite that places them.
    const Tag& sprite = targetMovie.sprite;
    Edit       header;
    header.offset = sprite.start;
    header.remove = sprite.bodyStart - sprite.start;
    header.insert = std::move(definitions);
    appendTagHeader(header.insert, kTagDefineSprite, spriteSize);
    edits.push_back(std::move(header));
    return applyEdits(target, std::move(edits));
}

std::string_view PromptMovieName(PromptMovie movie)
{
    return kPromptNames[static_cast<size_t>(movie)];
}

std::optional<PromptMovie> FindPromptMovie(std::string_view fileName)
{
    const size_t separator = fileName.find_last_of("/\\");
    if (separator != std::string_view::npos)
    {
        fileName.remove_prefix(separator + 1);
    }
    for (size_t i = 0; i < kPromptNames.size(); ++i)
    {
        if (equalsIgnoreCase(fileName, kPromptNames[i]))
        {
            return static_cast<PromptMovie>(i);
        }
    }
    return std::nullopt;
}

std::string_view RowsMovieName(Movie movie)
{
    return movie == Movie::WinLoseScreen ? kWinLoseRowsName : kHistographRowsName;
}

std::optional<Movie> FindRowsMovie(std::string_view fileName)
{
    // The loader resolves paths, so only the last component names the movie.
    const size_t separator = fileName.find_last_of("/\\");
    if (separator != std::string_view::npos)
    {
        fileName.remove_prefix(separator + 1);
    }
    for (const Movie movie : { Movie::WinLoseScreen, Movie::Histograph })
    {
        if (equalsIgnoreCase(fileName, RowsMovieName(movie)))
        {
            return movie;
        }
    }
    return std::nullopt;
}

Result PatchMovie(Movie movie, std::span<const uint8_t> source)
{
    return movie == Movie::WinLoseScreen ? patchWinLose(source) : patchHistograph(source);
}

namespace
{

std::filesystem::path gameRoot;

struct PreparedSlot
{
    std::once_flag once;
    Prepared       prepared;
};

std::array<PreparedSlot, 2>                                                        preparedSlots;
std::array<PreparedSlot, kPromptMovies>                                            promptSlots;
std::array<PreparedSlot, place_badges::kLastPlace - place_badges::kFirstPlace + 1> badgeSlots;

std::optional<std::vector<uint8_t>> readRetailEntry(std::string_view entry, std::string& reason)
{
    std::string error;
    auto        source = retail_ui_art::ReadFpkEntry(gameRoot / "Resource" / "Common" / kRetailArchive, entry, &error);
    if (!source)
    {
        reason = error.empty() ? std::string(entry) + " is unreadable" : error;
    }
    return source;
}

} // namespace

void SetGameRoot(std::filesystem::path root)
{
    gameRoot = std::move(root);
}

const Prepared& PreparedMovie(Movie movie)
{
    PreparedSlot& slot = preparedSlots[static_cast<size_t>(movie)];
    std::call_once(slot.once,
                   [&]
                   {
                       const auto source = readRetailEntry(
                           movie == Movie::WinLoseScreen ? kWinLoseEntry : kHistographEntry, slot.prepared.reason);
                       if (!source)
                       {
                           return;
                       }
                       Result result        = PatchMovie(movie, *source);
                       slot.prepared.bytes  = std::move(result.bytes);
                       slot.prepared.reason = result.reason;
                   });
    return slot.prepared;
}

const Prepared& PreparedPromptMovie(PromptMovie movie)
{
    PreparedSlot& slot = promptSlots[static_cast<size_t>(movie)];
    std::call_once(slot.once,
                   [&]
                   {
                       const auto target = readRetailEntry(PromptMovieName(movie), slot.prepared.reason);
                       if (!target)
                       {
                           return;
                       }
                       const auto donor = readRetailEntry(kPromptDonorEntry, slot.prepared.reason);
                       if (!donor)
                       {
                           return;
                       }
                       Result result        = PatchPromptMovie(*target, *donor);
                       slot.prepared.bytes  = std::move(result.bytes);
                       slot.prepared.reason = result.reason;
                   });
    return slot.prepared;
}

const Prepared& PreparedBadge(int place)
{
    static const Prepared kNone{ {}, "no such place badge" };
    if (place < place_badges::kFirstPlace || place > place_badges::kLastPlace)
    {
        return kNone;
    }
    PreparedSlot& slot = badgeSlots[static_cast<size_t>(place - place_badges::kFirstPlace)];
    std::call_once(slot.once,
                   [&]
                   {
                       const auto source = readRetailEntry(place_badges::kSourceImage, slot.prepared.reason);
                       if (!source)
                       {
                           return;
                       }
                       slot.prepared.bytes = place_badges::BuildBadge(*source, place);
                       if (slot.prepared.bytes.empty())
                       {
                           slot.prepared.reason = "place 5 badge differs from retail";
                       }
                   });
    return slot.prepared;
}

} // namespace rerevved::gfx_movie_rows
