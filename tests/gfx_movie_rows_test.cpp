#include "gfx_movie_rows.h"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace
{

using Bytes = std::vector<uint8_t>;
using rerevved::gfx_movie_rows::Movie;

constexpr uint16_t kTagEnd          = 0;
constexpr uint16_t kTagShowFrame    = 1;
constexpr uint16_t kTagPlace2       = 26;
constexpr uint16_t kTagPlace3       = 70;
constexpr uint16_t kTagDefineSprite = 39;
constexpr uint16_t kTagFrameLabel   = 43;
constexpr uint16_t kTagRemove2      = 28;
constexpr uint16_t kTagImage        = 1001;

struct Bits
{
    Bytes  bytes;
    size_t count = 0;

    void Put(unsigned width, uint32_t value)
    {
        for (unsigned i = width; i > 0; --i, ++count)
        {
            if (count % 8 == 0)
            {
                bytes.push_back(0);
            }
            bytes.back() |= static_cast<uint8_t>(((value >> (i - 1)) & 1) << (7 - count % 8));
        }
    }
};

void put16(Bytes& out, uint16_t value)
{
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8));
}

void put32(Bytes& out, uint32_t value)
{
    put16(out, static_cast<uint16_t>(value));
    put16(out, static_cast<uint16_t>(value >> 16));
}

Bytes tag(uint16_t code, const Bytes& body)
{
    Bytes out;
    if (body.size() < 63)
    {
        put16(out, static_cast<uint16_t>((code << 6) | body.size()));
    }
    else
    {
        put16(out, static_cast<uint16_t>((code << 6) | 63));
        put32(out, static_cast<uint32_t>(body.size()));
    }
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

Bytes place(uint16_t         depth,
            uint16_t         character,
            int32_t          x,
            int32_t          y,
            std::string_view name,
            bool             withTransform = true,
            uint16_t         clipDepth     = 0)
{
    Bytes body{ static_cast<uint8_t>((withTransform ? 0x2e : 0x26) | (clipDepth != 0 ? 0x40 : 0)) };
    put16(body, depth);
    put16(body, character);
    Bits matrix;
    matrix.Put(1, 0);
    matrix.Put(1, 0);
    matrix.Put(5, 16);
    matrix.Put(16, static_cast<uint32_t>(x) & 0xffff);
    matrix.Put(16, static_cast<uint32_t>(y) & 0xffff);
    body.insert(body.end(), matrix.bytes.begin(), matrix.bytes.end());
    Bits transform;
    transform.Put(1, 0);
    transform.Put(1, 1);
    transform.Put(4, 10);
    for (int i = 0; i < 4; ++i)
    {
        transform.Put(10, i == 3 ? 0 : 256);
    }
    if (withTransform)
    {
        body.insert(body.end(), transform.bytes.begin(), transform.bytes.end());
    }
    body.insert(body.end(), name.begin(), name.end());
    body.push_back(0);
    if (clipDepth != 0)
    {
        put16(body, clipDepth);
    }
    return tag(kTagPlace2, body);
}

Bytes movie(const std::vector<Bytes>& tags)
{
    Bytes out{ 'G', 'F', 'X', 8, 0, 0, 0, 0 };
    Bits  rect;
    rect.Put(5, 16);
    rect.Put(16, 0);
    rect.Put(16, 25600);
    rect.Put(16, 0);
    rect.Put(16, 14400);
    out.insert(out.end(), rect.bytes.begin(), rect.bytes.end());
    put16(out, 0x1e00);
    put16(out, 1);
    for (const Bytes& part : tags)
    {
        out.insert(out.end(), part.begin(), part.end());
    }
    const Bytes end = tag(kTagEnd, {});
    out.insert(out.end(), end.begin(), end.end());
    const size_t size = out.size();
    out[4]            = static_cast<uint8_t>(size);
    out[5]            = static_cast<uint8_t>(size >> 8);
    out[6]            = static_cast<uint8_t>(size >> 16);
    out[7]            = static_cast<uint8_t>(size >> 24);
    return out;
}

struct ParsedTag
{
    uint16_t code = 0;
    Bytes    body;
    Bytes    raw;
};

std::vector<ParsedTag> walk(const Bytes& data, size_t begin, size_t end)
{
    std::vector<ParsedTag> tags;
    size_t                 at = begin;
    while (at < end)
    {
        const uint16_t header = static_cast<uint16_t>(data[at] | (data[at + 1] << 8));
        size_t         size   = header & 63;
        size_t         body   = at + 2;
        if (size == 63)
        {
            size = data[body] | (data[body + 1] << 8) | (data[body + 2] << 16) |
                   (static_cast<size_t>(data[body + 3]) << 24);
            body += 4;
        }
        ParsedTag parsed;
        parsed.code = static_cast<uint16_t>(header >> 6);
        parsed.body.assign(data.begin() + static_cast<ptrdiff_t>(body),
                           data.begin() + static_cast<ptrdiff_t>(body + size));
        parsed.raw.assign(data.begin() + static_cast<ptrdiff_t>(at),
                          data.begin() + static_cast<ptrdiff_t>(body + size));
        tags.push_back(parsed);
        at = body + size;
    }
    return tags;
}

std::vector<ParsedTag> topLevel(const Bytes& data)
{
    const size_t rectBytes = (5 + 4 * static_cast<size_t>(data[8] >> 3) + 7) / 8;
    return walk(data, 8 + rectBytes + 4, data.size());
}

struct Placed
{
    uint16_t    depth     = 0;
    uint16_t    character = 0;
    bool        scaled    = false;
    int32_t     scale     = 0;
    int32_t     x         = 0;
    int32_t     y         = 0;
    std::string name;
};

uint32_t readBits(const Bytes& data, size_t& bit, unsigned width)
{
    uint32_t value = 0;
    for (unsigned i = 0; i < width; ++i, ++bit)
    {
        value = (value << 1) | ((data[bit >> 3] >> (7 - (bit & 7))) & 1);
    }
    return value;
}

int32_t readSigned(const Bytes& data, size_t& bit, unsigned width)
{
    uint32_t value = readBits(data, bit, width);
    if ((value >> (width - 1)) & 1)
    {
        value |= ~uint32_t{ 0 } << width;
    }
    return static_cast<int32_t>(value);
}

Placed decodePlace(const Bytes& body)
{
    Placed placed;
    placed.depth     = static_cast<uint16_t>(body[1] | (body[2] << 8));
    placed.character = static_cast<uint16_t>(body[3] | (body[4] << 8));
    size_t bit       = 5 * 8;
    placed.scaled    = readBits(body, bit, 1) != 0;
    if (placed.scaled)
    {
        const unsigned width = readBits(body, bit, 5);
        placed.scale         = readSigned(body, bit, width);
        readSigned(body, bit, width);
    }
    readBits(body, bit, 1);
    const unsigned width = readBits(body, bit, 5);
    placed.x             = readSigned(body, bit, width);
    placed.y             = readSigned(body, bit, width);

    const size_t transformStart = (bit + 7) / 8;
    bit                         = transformStart * 8 + 2;
    const size_t transformBytes = (6 + size_t{ readBits(body, bit, 4) } * 4 + 7) / 8;
    placed.name.assign(body.begin() + static_cast<ptrdiff_t>(transformStart + transformBytes),
                       body.end() - 1);
    return placed;
}

std::vector<Placed> places(const std::vector<ParsedTag>& tags)
{
    std::vector<Placed> out;
    for (const ParsedTag& parsed : tags)
    {
        if (parsed.code == kTagPlace2)
        {
            out.push_back(decodePlace(parsed.body));
        }
    }
    return out;
}

Bytes label(std::string_view text)
{
    Bytes body(text.begin(), text.end());
    body.push_back(0);
    return tag(kTagFrameLabel, body);
}

Bytes sprite(uint16_t id, uint16_t frames, const std::vector<Bytes>& children)
{
    Bytes body;
    put16(body, id);
    put16(body, frames);
    for (const Bytes& child : children)
    {
        body.insert(body.end(), child.begin(), child.end());
    }
    const Bytes end = tag(kTagEnd, {});
    body.insert(body.end(), end.begin(), end.end());
    return tag(kTagDefineSprite, body);
}

Bytes image(uint16_t id, std::string_view name)
{
    Bytes body;
    put16(body, id);
    put16(body, 14);
    put16(body, 64);
    put16(body, 64);
    const std::string file = std::string(name) + ".dds";
    for (std::string_view text : { name, std::string_view(file) })
    {
        body.push_back(static_cast<uint8_t>(text.size()));
        body.insert(body.end(), text.begin(), text.end());
    }
    return tag(kTagImage, body);
}

Bytes badgePlace(uint16_t character)
{
    Bytes body{ 0x16 };
    put16(body, 1);
    put16(body, character);
    body.insert(body.end(), { 0x00, 0x00, 0x00 });
    return tag(kTagPlace2, body);
}

// Images 22 and 23 (Ribbon4, Ribbon5), shape 223 filled with 23, sprite 224, clip 225 with
// place_1..place_5. Variant 1 also labels place_6; variant 2 names image 23 Ribbon9.
std::vector<Bytes> badgeTags(int variant = 0)
{
    const Bytes        shape = tag(2, { 0xdf, 0x00, 0x00, 0x01, 0x41, 0x17, 0x00, 0x00, 0x00, 0x00 });
    std::vector<Bytes> frames;
    for (int place = 1; place <= 4; ++place)
    {
        frames.push_back(label("place_" + std::to_string(place)));
        frames.push_back(tag(kTagShowFrame, {}));
    }
    frames.push_back(tag(kTagRemove2, { 0x01, 0x00 }));
    frames.push_back(label("place_5"));
    frames.push_back(badgePlace(224));
    frames.push_back(tag(kTagShowFrame, {}));
    frames.push_back(tag(kTagShowFrame, {}));
    if (variant == 1)
    {
        frames.push_back(label("place_6"));
        frames.push_back(tag(kTagShowFrame, {}));
    }
    return { image(22, "Ribbon4"),
             image(23, variant == 2 ? "Ribbon9" : "Ribbon5"),
             shape,
             sprite(224, 1, { badgePlace(223), tag(kTagShowFrame, {}) }),
             sprite(225, variant == 1 ? 7 : 6, frames) };
}

// extraRow: 0 none, 1 an existing port5 in the row form, 2 one without a colour transform.
// maskEnd, when set, adds a mask at depth 150 that hides depths up to maskEnd.
Bytes winLoseMovie(int32_t                   port3Y           = 9586,
                   int                       extraRow         = 0,
                   bool                      frameBeforePort4 = false,
                   uint16_t                  maskEnd          = 0,
                   bool                      endFrame         = true,
                   const std::vector<Bytes>& badges           = {})
{
    std::vector<Bytes> tags = badges;
    if (maskEnd != 0)
    {
        tags.push_back(place(150, 300, 0, 0, "mask", true, maskEnd));
    }
    tags.push_back(place(1, 182, 21369, 13155, "buttonHelp"));
    tags.push_back(place(17, 236, 6607, 8119, "port2"));
    tags.push_back(place(46, 236, 6607, 6652, "port1"));
    tags.push_back(place(75, 236, 6607, port3Y, "port3"));
    if (frameBeforePort4)
    {
        tags.push_back(tag(kTagShowFrame, {}));
    }
    tags.push_back(place(104, 236, 6607, 11053, "port4"));
    if (extraRow != 0)
    {
        tags.push_back(place(120, 236, 6607, 12520, "port5", extraRow == 1));
    }
    tags.push_back(place(133, 248, 12800, 4559, "theTopBar"));
    tags.push_back(place(163, 266, 8686, 1818, "port0"));
    if (endFrame)
    {
        tags.push_back(tag(kTagShowFrame, {}));
    }
    return movie(tags);
}

// maskEnd, when set, adds a mask at depth 3 that hides depths up to maskEnd.
Bytes histographMovie(int32_t  row3Y           = 2794,
                      bool     plainRow7       = false,
                      bool     frameBeforeRow6 = false,
                      uint16_t maskEnd         = 0,
                      bool     endFrame        = true)
{
    std::vector<Bytes> children;
    if (maskEnd != 0)
    {
        children.push_back(place(3, 300, 0, 0, "mask", true, maskEnd));
    }
    const int32_t ys[] = { 0, 1397, row3Y, 4191, 5588, 6985 };
    for (int i = 0; i < 6; ++i)
    {
        if (i == 5 && frameBeforeRow6)
        {
            children.push_back(tag(kTagShowFrame, {}));
        }
        children.push_back(place(static_cast<uint16_t>(1 + 5 * i),
                                 194,
                                 -1423,
                                 ys[i],
                                 "playerInfo" + std::to_string(i + 1)));
    }
    if (plainRow7)
    {
        children.push_back(place(31, 194, -1423, 8382, "playerInfo7", false));
    }
    if (endFrame)
    {
        children.push_back(tag(kTagShowFrame, {}));
    }
    children.push_back(tag(kTagEnd, {}));
    Bytes spriteBody{ 195, 0, 1, 0 };
    for (const Bytes& child : children)
    {
        spriteBody.insert(spriteBody.end(), child.begin(), child.end());
    }
    return movie({ tag(kTagDefineSprite, spriteBody),
                   place(70, 195, 6177, 2663, "playerInfoPanel"),
                   tag(kTagShowFrame, {}) });
}

uint16_t placedCharacter(const ParsedTag& placed)
{
    return static_cast<uint16_t>(placed.body[3] | (placed.body[4] << 8));
}

bool refused(Movie kind, const Bytes& source)
{
    const auto result = rerevved::gfx_movie_rows::PatchMovie(kind, source);
    return result.bytes.empty() && !result.reason.empty();
}

} // namespace

TEST_CASE("gfx_movie_rows_find_movie", "[ctest][gfx-movie-rows]")
{
    using rerevved::gfx_movie_rows::FindRowsMovie;
    using rerevved::gfx_movie_rows::RowsMovieName;
    CHECK((FindRowsMovie("gfx_winlosescreenrows.gfx") == Movie::WinLoseScreen));
    CHECK((FindRowsMovie("ui\\sub/GFX_HISTOGRAPHROWS.GFX") == Movie::Histograph));
    CHECK((FindRowsMovie(RowsMovieName(Movie::WinLoseScreen)) == Movie::WinLoseScreen));
    CHECK((FindRowsMovie(RowsMovieName(Movie::Histograph)) == Movie::Histograph));
    CHECK(!FindRowsMovie("GFX_WinLoseScreen.gfx"));
    CHECK(!FindRowsMovie("GFX_Histograph.gfx"));
    CHECK(!FindRowsMovie("ui/"));
}

TEST_CASE("gfx_movie_rows_win_lose_adds_place_rows", "[ctest][gfx-movie-rows]")
{
    const Bytes source = winLoseMovie();
    const auto  result = rerevved::gfx_movie_rows::PatchMovie(Movie::WinLoseScreen, source);
    REQUIRE(!result.bytes.empty());
    CHECK(result.reason.empty());

    const Bytes& patched = result.bytes;
    CHECK((patched[4] | (patched[5] << 8) | (patched[6] << 16)) ==
          static_cast<int>(patched.size()));

    const auto          tags = topLevel(patched);
    const auto          rows = places(tags);
    std::vector<Placed> ports;
    for (const Placed& placed : rows)
    {
        if (placed.name.rfind("port", 0) == 0 && placed.name != "port0")
        {
            ports.push_back(placed);
        }
    }
    REQUIRE(ports.size() == 6);

    const std::string names[] = { "port1", "port2", "port3", "port4", "port5", "port6" };
    for (int i = 0; i < 6; ++i)
    {
        const Placed* row = nullptr;
        for (const Placed& candidate : ports)
        {
            if (candidate.name == names[i])
            {
                row = &candidate;
            }
        }
        REQUIRE(row != nullptr);
        UNSCOPED_INFO(names[i]);
        CHECK(row->character == 236);
        CHECK(row->scaled);
        CHECK(row->scale == 41943);
        CHECK(row->x == 8786);
        CHECK(row->y == 6598 + 980 * i);
    }
    UNSCOPED_INFO("new rows take unused depths");
    for (const Placed& placed : ports)
    {
        if (placed.name == "port5")
        {
            CHECK(placed.depth == 164);
        }
        if (placed.name == "port6")
        {
            CHECK(placed.depth == 165);
        }
    }

    const auto sourceTags = topLevel(source);
    REQUIRE(tags.size() == sourceTags.size() + 2);

    UNSCOPED_INFO("other placements and the frame tags are copied as they were");
    CHECK((tags[0].raw == sourceTags[0].raw));
    CHECK((tags[tags.size() - 2].raw == sourceTags[sourceTags.size() - 2].raw));
}

TEST_CASE("gfx_movie_rows_win_lose_adds_place_badges", "[ctest][gfx-movie-rows]")
{
    const Bytes source = winLoseMovie(9586, 0, false, 0, true, badgeTags());
    const auto  result = rerevved::gfx_movie_rows::PatchMovie(Movie::WinLoseScreen, source);
    REQUIRE(!result.bytes.empty());

    const auto tags = topLevel(result.bytes);
    REQUIRE(tags.size() >= 11);
    UNSCOPED_INFO("each new place gets an image, shape and sprite before the clip");
    const uint16_t ids[] = { 226, 227, 228, 229, 230, 231 };
    for (int place = 0; place < 2; ++place)
    {
        const ParsedTag&  newImage  = tags[4 + 3 * place];
        const ParsedTag&  newShape  = tags[5 + 3 * place];
        const ParsedTag&  newSprite = tags[6 + 3 * place];
        const uint16_t    imageId   = ids[3 * place];
        const uint16_t    shapeId   = ids[3 * place + 1];
        const uint16_t    spriteId  = ids[3 * place + 2];
        const std::string name      = "Ribbon" + std::to_string(6 + place);
        REQUIRE(newImage.code == kTagImage);
        CHECK((newImage.body[0] | (newImage.body[1] << 8)) == imageId);
        CHECK(std::string(newImage.body.begin() + 9, newImage.body.begin() + 9 + newImage.body[8]) == name);
        CHECK(std::string(newImage.body.end() - 11, newImage.body.end()) == name + ".dds");
        REQUIRE(newShape.code == 2);
        CHECK((newShape.body[0] | (newShape.body[1] << 8)) == shapeId);
        CHECK((newShape.body[5] | (newShape.body[6] << 8)) == imageId);
        REQUIRE(newSprite.code == kTagDefineSprite);
        CHECK((newSprite.body[0] | (newSprite.body[1] << 8)) == spriteId);
        CHECK(placedCharacter(walk(newSprite.body, 4, newSprite.body.size())[0]) == shapeId);
    }

    const ParsedTag& clip = tags[10];
    REQUIRE(clip.code == kTagDefineSprite);
    CHECK((clip.body[0] | (clip.body[1] << 8)) == 225);
    CHECK((clip.body[2] | (clip.body[3] << 8)) == 10);
    const auto            inner = walk(clip.body, 4, clip.body.size());
    std::vector<uint16_t> codes;
    for (const ParsedTag& child : inner)
    {
        codes.push_back(child.code);
    }
    const std::vector<uint16_t> added = { kTagRemove2, kTagFrameLabel, kTagPlace2, kTagShowFrame, kTagShowFrame };
    REQUIRE(codes.size() == 13 + 2 * added.size() + 1);
    for (int place = 0; place < 2; ++place)
    {
        UNSCOPED_INFO(place);
        const size_t at = 13 + added.size() * place;
        CHECK(std::vector<uint16_t>(codes.begin() + at, codes.begin() + at + added.size()) == added);
        CHECK(std::string(inner[at + 1].body.begin(), inner[at + 1].body.end() - 1) ==
              "place_" + std::to_string(6 + place));
        CHECK(placedCharacter(inner[at + 2]) == ids[3 * place + 2]);
    }
    CHECK(codes.back() == kTagEnd);
}

TEST_CASE("gfx_movie_rows_histograph_adds_legend_rows", "[ctest][gfx-movie-rows]")
{
    const Bytes source = histographMovie();
    const auto  result = rerevved::gfx_movie_rows::PatchMovie(Movie::Histograph, source);
    REQUIRE(!result.bytes.empty());

    const Bytes& patched = result.bytes;
    CHECK((patched[4] | (patched[5] << 8) | (patched[6] << 16)) ==
          static_cast<int>(patched.size()));

    const auto tags = topLevel(patched);
    REQUIRE(tags.size() == 4);
    REQUIRE(tags[0].code == kTagDefineSprite);
    const Bytes& body  = tags[0].body;
    const auto   inner = walk(body, 4, body.size());
    const auto   rows  = places(inner);
    REQUIRE(rows.size() == 8);
    for (int i = 0; i < 8; ++i)
    {
        UNSCOPED_INFO(i);
        CHECK(rows[i].name == "playerInfo" + std::to_string(i + 1));
        CHECK(rows[i].character == 194);
        CHECK(rows[i].depth == 1 + 5 * i);
        CHECK(!rows[i].scaled);
        CHECK(rows[i].x == -1423);
        CHECK(rows[i].y == 1397 * i);
    }
    UNSCOPED_INFO("the sprite still ends with its frame and End tags");
    CHECK(inner[inner.size() - 2].code == kTagShowFrame);
    CHECK(inner.back().code == kTagEnd);
    UNSCOPED_INFO("the panel placement and frame are copied as they were");
    const auto sourceTags = topLevel(source);
    CHECK((tags[1].raw == sourceTags[1].raw));
    CHECK((tags[2].raw == sourceTags[2].raw));
}

TEST_CASE("gfx_movie_rows_refuses_non_retail_structure", "[ctest][gfx-movie-rows]")
{
    UNSCOPED_INFO("declared length that differs from the bytes");
    Bytes shortLength = winLoseMovie();
    shortLength[4]    = static_cast<uint8_t>(shortLength[4] - 1);
    CHECK(refused(Movie::WinLoseScreen, shortLength));

    UNSCOPED_INFO("a row that is not at its retail spacing");
    CHECK(refused(Movie::WinLoseScreen, winLoseMovie(9587)));

    UNSCOPED_INFO("a movie that already holds a fifth row");
    CHECK(refused(Movie::WinLoseScreen, winLoseMovie(9586, 1)));

    UNSCOPED_INFO("an existing row named in a form the patch does not write");
    CHECK(refused(Movie::WinLoseScreen, winLoseMovie(9586, 2)));
    CHECK(refused(Movie::Histograph, histographMovie(2794, true)));

    UNSCOPED_INFO("a patched movie is not patched twice");
    const auto once = rerevved::gfx_movie_rows::PatchMovie(Movie::WinLoseScreen, winLoseMovie());
    REQUIRE(!once.bytes.empty());
    CHECK(refused(Movie::WinLoseScreen, once.bytes));
    const auto twice = rerevved::gfx_movie_rows::PatchMovie(Movie::Histograph, histographMovie());
    REQUIRE(!twice.bytes.empty());
    CHECK(refused(Movie::Histograph, twice.bytes));

    UNSCOPED_INFO("rows that are not all in the first frame");
    CHECK(refused(Movie::WinLoseScreen, winLoseMovie(9586, 0, true)));
    CHECK(refused(Movie::Histograph, histographMovie(2794, false, true)));

    UNSCOPED_INFO("a mask whose range reaches the added depths");
    CHECK(refused(Movie::WinLoseScreen, winLoseMovie(9586, 0, false, 170)));
    CHECK(refused(Movie::Histograph, histographMovie(2794, false, false, 40)));
    UNSCOPED_INFO("a mask that ends below them still patches");
    CHECK(!refused(Movie::WinLoseScreen, winLoseMovie(9586, 0, false, 163)));
    CHECK(!refused(Movie::Histograph, histographMovie(2794, false, false, 30)));

    UNSCOPED_INFO("a timeline that never ends a frame");
    CHECK(refused(Movie::WinLoseScreen, winLoseMovie(9586, 0, false, 0, false)));
    CHECK(refused(Movie::Histograph, histographMovie(2794, false, false, 0, false)));

    UNSCOPED_INFO("legend rows off their retail spacing");
    CHECK(refused(Movie::Histograph, histographMovie(2795)));

    UNSCOPED_INFO("the other movie's structure");
    CHECK(refused(Movie::Histograph, winLoseMovie()));
    CHECK(refused(Movie::WinLoseScreen, histographMovie()));

    UNSCOPED_INFO("a placement tag the patch cannot account for");
    Bytes       withPlace3 = winLoseMovie();
    const Bytes extra      = tag(kTagPlace3, Bytes(8, 0));
    withPlace3.insert(withPlace3.end() - 2, extra.begin(), extra.end());
    withPlace3[4] = static_cast<uint8_t>(withPlace3.size());
    withPlace3[5] = static_cast<uint8_t>(withPlace3.size() >> 8);
    CHECK(refused(Movie::WinLoseScreen, withPlace3));

    UNSCOPED_INFO("a tag stream without its End tag");
    Bytes noEnd = winLoseMovie();
    noEnd.resize(noEnd.size() - 2);
    noEnd[4] = static_cast<uint8_t>(noEnd.size());
    noEnd[5] = static_cast<uint8_t>(noEnd.size() >> 8);
    CHECK(refused(Movie::WinLoseScreen, noEnd));

    UNSCOPED_INFO("place badges that already go past place 5 or name another image");
    CHECK(refused(Movie::WinLoseScreen, winLoseMovie(9586, 0, false, 0, true, badgeTags(1))));
    CHECK(refused(Movie::WinLoseScreen, winLoseMovie(9586, 0, false, 0, true, badgeTags(2))));

    UNSCOPED_INFO("not a GFX movie");
    Bytes notGfx = winLoseMovie();
    notGfx[0]    = 'C';
    CHECK(refused(Movie::WinLoseScreen, notGfx));
    CHECK(refused(Movie::Histograph, Bytes{}));
}
