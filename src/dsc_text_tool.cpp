#include "dsc_text_tool.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hazuki::dsc {
namespace {

constexpr wchar_t kProjectSuffix[] = L".hazuki.txt";
constexpr wchar_t kProjectMagic[] = L"# BGI_HAZUKI_DSC_TEXT_V1";
constexpr std::uint32_t kDefaultCodepage = 932;
constexpr std::size_t kDscSymbolCount = 512;
constexpr std::size_t kDscLiteralCount = 256;
constexpr std::size_t kDscMaxDistance = 4097;
constexpr std::size_t kDscMinMatchLength = 3;
constexpr std::size_t kDscMaxMatchLength = 257;
constexpr std::size_t kMatchHashSize = 1u << 16;
constexpr int kMaxHashChainChecks = 64;
constexpr std::array<std::uint8_t, 17> kDscMagic = {
    0x44, 0x53, 0x43, 0x20, 0x46, 0x4F, 0x52, 0x4D, 0x41,
    0x54, 0x20, 0x31, 0x2E, 0x30, 0x30, 0x00, 0x00,
};
constexpr char kCompiledMagic[] = "BurikoCompiledScriptVer1.00\0";

struct ScriptConfig {
    std::uint32_t header_size = 0;
    std::optional<std::uint32_t> additional_header_pos;
    std::uint32_t string_type = 0x3;
    std::uint32_t file_type = 0x7F;
    std::uint32_t text_function = 0x140;
    std::uint32_t backlog_function = 0x143;
    std::uint32_t ruby_function = 0x14B;
    std::int32_t name_pos = 0;
    std::int32_t text_pos = 0;
    std::int32_t ruby_kanji_pos = 0;
    std::int32_t ruby_furigana_pos = 0;
    std::int32_t backlog_pos = 0;
};

struct TextSlot {
    std::uint32_t offset = 0;
    std::vector<std::uint8_t> bytes;
};

struct ScriptLayout {
    ScriptContainerKind container_kind = ScriptContainerKind::DscCompressed;
    ScriptConfig config;
    std::vector<std::uint8_t> compiled_data;
    std::vector<std::uint8_t> header_bytes;
    std::vector<std::uint8_t> code_bytes;
    std::vector<TextSlot> text_slots;
};

struct DscNode {
    bool has_children = false;
    bool look_behind = false;
    std::uint8_t value = 0;
    std::uint32_t children[2] = {0, 0};
};

struct DscBitCode {
    std::vector<bool> bits;
};

struct MatchInfo {
    std::uint16_t length = 0;
    std::uint16_t distance = 0;
};

struct EncodedToken {
    std::uint16_t symbol = 0;
    std::uint16_t distance = 0;
    std::uint16_t length = 1;
};

class ByteReader {
public:
    explicit ByteReader(std::vector<std::uint8_t> data) : data_(std::move(data)) {}

    std::uint8_t ReadU8() {
        EnsureAvailable(1);
        return data_[position_++];
    }

    std::uint32_t ReadU32() {
        EnsureAvailable(4);
        const auto value = static_cast<std::uint32_t>(data_[position_])
            | (static_cast<std::uint32_t>(data_[position_ + 1]) << 8)
            | (static_cast<std::uint32_t>(data_[position_ + 2]) << 16)
            | (static_cast<std::uint32_t>(data_[position_ + 3]) << 24);
        position_ += 4;
        return value;
    }

    std::vector<std::uint8_t> ReadBytes(std::size_t count) {
        EnsureAvailable(count);
        std::vector<std::uint8_t> chunk(
            data_.begin() + static_cast<std::ptrdiff_t>(position_),
            data_.begin() + static_cast<std::ptrdiff_t>(position_ + count));
        position_ += count;
        return chunk;
    }

    void Skip(std::size_t count) {
        EnsureAvailable(count);
        position_ += count;
    }

    std::size_t Remaining() const {
        return data_.size() - position_;
    }

    const std::vector<std::uint8_t> &Data() const {
        return data_;
    }

private:
    void EnsureAvailable(std::size_t count) const {
        if (position_ + count > data_.size()) {
            throw std::runtime_error("Unexpected end of data.");
        }
    }

    std::vector<std::uint8_t> data_;
    std::size_t position_ = 0;
};

class BitReader {
public:
    explicit BitReader(const std::vector<std::uint8_t> &data) : data_(data) {}

    bool ReadBit() {
        if (byte_index_ >= data_.size()) {
            throw std::runtime_error("Unexpected end of bitstream.");
        }
        const bool bit = (data_[byte_index_] & (0x80u >> bit_index_)) != 0;
        ++bit_index_;
        if (bit_index_ == 8) {
            bit_index_ = 0;
            ++byte_index_;
        }
        return bit;
    }

private:
    const std::vector<std::uint8_t> &data_;
    std::size_t byte_index_ = 0;
    int bit_index_ = 0;
};

class BitWriter {
public:
    void WriteBits(const std::vector<bool> &bits) {
        for (const bool bit : bits) {
            current_byte_ |= static_cast<std::uint8_t>((bit ? 1 : 0) << (7 - bit_index_));
            ++bit_index_;
            if (bit_index_ == 8) {
                FlushByte();
            }
        }
    }

    void WriteValue(std::uint32_t value, int bit_count) {
        for (int i = bit_count - 1; i >= 0; --i) {
            current_byte_ |= static_cast<std::uint8_t>(((value >> i) & 1u) << (7 - bit_index_));
            ++bit_index_;
            if (bit_index_ == 8) {
                FlushByte();
            }
        }
    }

    std::vector<std::uint8_t> Finish() {
        if (bit_index_ != 0) {
            FlushByte();
        }
        return std::move(bytes_);
    }

private:
    void FlushByte() {
        bytes_.push_back(current_byte_);
        current_byte_ = 0;
        bit_index_ = 0;
    }

    std::vector<std::uint8_t> bytes_;
    std::uint8_t current_byte_ = 0;
    int bit_index_ = 0;
};

struct HuffmanBuildNode {
    std::uint64_t frequency = 0;
    int symbol = -1;
    int left = -1;
    int right = -1;
    std::uint64_t order = 0;
};

struct ExtractedEntryInfo {
    TextKind kind = TextKind::Other;
    std::wstring comment;
    std::vector<std::uint32_t> code_offsets;
};

std::vector<std::uint8_t> ReadAllBytes(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Failed to open file.");
    }
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

void WriteAllBytes(const std::filesystem::path &path, const std::vector<std::uint8_t> &bytes) {
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Failed to create output file.");
    }
    file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file) {
        throw std::runtime_error("Failed to write output file.");
    }
}

std::uint32_t ReadU32(const std::vector<std::uint8_t> &data, std::size_t offset) {
    if (offset + 4 > data.size()) {
        return 0;
    }
    return static_cast<std::uint32_t>(data[offset])
        | (static_cast<std::uint32_t>(data[offset + 1]) << 8)
        | (static_cast<std::uint32_t>(data[offset + 2]) << 16)
        | (static_cast<std::uint32_t>(data[offset + 3]) << 24);
}

void WriteU32(std::vector<std::uint8_t> &data, std::size_t offset, std::uint32_t value) {
    if (offset + 4 > data.size()) {
        throw std::runtime_error("Attempted to write beyond the code buffer.");
    }
    data[offset] = static_cast<std::uint8_t>(value & 0xFFu);
    data[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
    data[offset + 2] = static_cast<std::uint8_t>((value >> 16) & 0xFFu);
    data[offset + 3] = static_cast<std::uint8_t>((value >> 24) & 0xFFu);
}

void AppendU32(std::vector<std::uint8_t> &data, std::uint32_t value) {
    data.push_back(static_cast<std::uint8_t>(value & 0xFFu));
    data.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
    data.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFFu));
    data.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFFu));
}

bool StartsWith(const std::vector<std::uint8_t> &bytes, const char *literal) {
    const auto length = std::strlen(literal) + 1;
    return bytes.size() >= length
        && std::equal(reinterpret_cast<const std::uint8_t *>(literal), reinterpret_cast<const std::uint8_t *>(literal) + length, bytes.begin());
}

bool EndsWithInsensitive(const std::wstring &value, const std::wstring &suffix) {
    if (suffix.size() > value.size()) {
        return false;
    }
    return std::equal(suffix.rbegin(), suffix.rend(), value.rbegin(), [](wchar_t left, wchar_t right) {
        return std::towlower(left) == std::towlower(right);
    });
}

std::wstring WideToUtf8WideHack(const std::wstring &value) {
    return value;
}

std::string WideToUtf8(const std::wstring &value) {
    if (value.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        throw std::runtime_error("Failed to encode text as UTF-8.");
    }
    std::string output(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), output.data(), size, nullptr, nullptr);
    return output;
}

std::wstring Utf8ToWide(const std::string &value) {
    if (value.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) {
        throw std::runtime_error("Failed to decode UTF-8 text file.");
    }
    std::wstring output(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), output.data(), size);
    return output;
}

std::wstring EscapeProjectText(const std::wstring &value) {
    std::wstring escaped;
    escaped.reserve(value.size());
    for (const wchar_t ch : value) {
        switch (ch) {
        case L'\\':
            escaped += L"\\\\";
            break;
        case L'\n':
            escaped += L"\\n";
            break;
        case L'\r':
            escaped += L"\\r";
            break;
        case L'\t':
            escaped += L"\\t";
            break;
        default:
            escaped.push_back(ch);
            break;
        }
    }
    return escaped;
}

std::wstring UnescapeProjectText(const std::wstring &value) {
    std::wstring output;
    output.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        const auto ch = value[i];
        if (ch == L'\\' && i + 1 < value.size()) {
            const auto next = value[++i];
            switch (next) {
            case L'n':
                output.push_back(L'\n');
                break;
            case L'r':
                output.push_back(L'\r');
                break;
            case L't':
                output.push_back(L'\t');
                break;
            case L'\\':
                output.push_back(L'\\');
                break;
            default:
                output.push_back(next);
                break;
            }
        } else {
            output.push_back(ch);
        }
    }
    return output;
}

std::wstring Trim(const std::wstring &value) {
    const auto first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(L" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::vector<std::wstring> SplitLines(const std::wstring &value) {
    std::vector<std::wstring> lines;
    std::wstring current;
    for (const wchar_t ch : value) {
        if (ch == L'\r') {
            continue;
        }
        if (ch == L'\n') {
            lines.push_back(current);
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    lines.push_back(current);
    return lines;
}

std::wstring JoinHexOffsets(const std::vector<std::uint32_t> &offsets) {
    std::wostringstream stream;
    for (std::size_t i = 0; i < offsets.size(); ++i) {
        if (i != 0) {
            stream << L",";
        }
        stream << std::uppercase << std::hex << std::setw(8) << std::setfill(L'0') << offsets[i];
    }
    return stream.str();
}

std::vector<std::uint32_t> ParseHexOffsets(const std::wstring &value) {
    std::vector<std::uint32_t> offsets;
    std::wstringstream stream(value);
    std::wstring token;
    while (std::getline(stream, token, L',')) {
        token = Trim(token);
        if (token.empty()) {
            continue;
        }
        offsets.push_back(static_cast<std::uint32_t>(std::stoul(token, nullptr, 16)));
    }
    return offsets;
}

const ScriptConfig &GetVer000Config() {
    static const ScriptConfig config = {
        0x0,
        std::nullopt,
        0x3,
        0x7F,
        0x140,
        0x143,
        0x14B,
        0x24,
        0x2C,
        0x14,
        0x0C,
        0x0C,
    };
    return config;
}

const ScriptConfig &GetVer100Config() {
    static const ScriptConfig config = {
        0x1C,
        0x1C,
        0x3,
        0x7F,
        0x140,
        0x143,
        0x14B,
        0x0C,
        0x04,
        0x04,
        0x0C,
        0x0C,
    };
    return config;
}

std::wstring FormatHexEscape(std::uint16_t value) {
    std::wostringstream stream;
    stream << L"&#" << std::uppercase << std::hex << std::setw(4) << std::setfill(L'0') << value;
    return stream.str();
}

std::wstring DecodeScriptBytes(const std::vector<std::uint8_t> &bytes, std::uint32_t codepage) {
    std::wstring output;
    for (std::size_t i = 0; i < bytes.size();) {
        const bool lead = IsDBCSLeadByteEx(codepage, bytes[i]) != FALSE;
        const int chunk_size = (lead && i + 1 < bytes.size()) ? 2 : 1;
        int wide_count = MultiByteToWideChar(
            codepage,
            MB_ERR_INVALID_CHARS,
            reinterpret_cast<const char *>(bytes.data() + i),
            chunk_size,
            nullptr,
            0);
        if (wide_count > 0) {
            std::wstring chunk(static_cast<std::size_t>(wide_count), L'\0');
            MultiByteToWideChar(
                codepage,
                MB_ERR_INVALID_CHARS,
                reinterpret_cast<const char *>(bytes.data() + i),
                chunk_size,
                chunk.data(),
                wide_count);
            output += chunk;
        } else if (chunk_size == 2) {
            output += FormatHexEscape(static_cast<std::uint16_t>((bytes[i] << 8) | bytes[i + 1]));
        } else {
            output += FormatHexEscape(bytes[i]);
        }
        i += static_cast<std::size_t>(chunk_size);
    }
    return output;
}

bool IsHexDigit(wchar_t ch) {
    return (ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'f') || (ch >= L'A' && ch <= L'F');
}

int HexValue(wchar_t ch) {
    if (ch >= L'0' && ch <= L'9') {
        return ch - L'0';
    }
    if (ch >= L'a' && ch <= L'f') {
        return ch - L'a' + 10;
    }
    if (ch >= L'A' && ch <= L'F') {
        return ch - L'A' + 10;
    }
    return 0;
}

std::vector<std::uint8_t> EncodeWideChunk(const std::wstring &chunk, std::uint32_t codepage) {
    if (chunk.empty()) {
        return {};
    }
    BOOL used_default = FALSE;
    const int size = WideCharToMultiByte(
        codepage,
        WC_NO_BEST_FIT_CHARS,
        chunk.c_str(),
        static_cast<int>(chunk.size()),
        nullptr,
        0,
        nullptr,
        &used_default);
    if (size <= 0 || used_default) {
        throw std::runtime_error("A translation contains characters that cannot be encoded in the selected code page.");
    }
    std::vector<std::uint8_t> output(static_cast<std::size_t>(size));
    WideCharToMultiByte(
        codepage,
        WC_NO_BEST_FIT_CHARS,
        chunk.c_str(),
        static_cast<int>(chunk.size()),
        reinterpret_cast<char *>(output.data()),
        size,
        nullptr,
        &used_default);
    if (used_default) {
        throw std::runtime_error("A translation contains characters that cannot be encoded in the selected code page.");
    }
    return output;
}

std::vector<std::uint8_t> EncodeScriptText(const std::wstring &text, std::uint32_t codepage) {
    std::vector<std::uint8_t> output;
    std::wstring chunk;
    auto flush_chunk = [&]() {
        auto bytes = EncodeWideChunk(chunk, codepage);
        output.insert(output.end(), bytes.begin(), bytes.end());
        chunk.clear();
    };

    for (std::size_t i = 0; i < text.size();) {
        if (text[i] == L'&' && i + 5 < text.size() && text[i + 1] == L'#'
            && IsHexDigit(text[i + 2]) && IsHexDigit(text[i + 3])
            && IsHexDigit(text[i + 4]) && IsHexDigit(text[i + 5])) {
            flush_chunk();
            const std::uint16_t value = static_cast<std::uint16_t>(
                (HexValue(text[i + 2]) << 12)
                | (HexValue(text[i + 3]) << 8)
                | (HexValue(text[i + 4]) << 4)
                | HexValue(text[i + 5]));
            if ((value >> 8) != 0) {
                output.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
            }
            output.push_back(static_cast<std::uint8_t>(value & 0xFFu));
            i += 6;
            continue;
        }
        chunk.push_back(text[i]);
        ++i;
    }
    flush_chunk();
    return output;
}

std::uint8_t NextKeyByte(std::uint32_t &key) {
    const std::uint32_t v0 = 20021u * (key & 0xFFFFu);
    std::uint32_t v1 = key >> 16;
    v1 = v1 * 20021u + key * 346u;
    v1 = (v1 + (v0 >> 16)) & 0xFFFFu;
    key = (v1 << 16) + (v0 & 0xFFFFu) + 1u;
    return static_cast<std::uint8_t>(v1 & 0xFFu);
}

std::array<DscNode, 1024> BuildDscTree(const std::array<std::uint8_t, 512> &lengths) {
    std::array<DscNode, 1024> nodes{};
    std::vector<std::uint32_t> entries;
    entries.reserve(512);
    for (std::uint32_t n = 0; n < 512; ++n) {
        const auto length = lengths[n];
        if (length != 0) {
            entries.push_back((static_cast<std::uint32_t>(length) << 16) + n);
        }
    }
    std::sort(entries.begin(), entries.end());

    std::array<std::uint32_t, 1024> arr1{};
    std::uint32_t unk0 = 0x200;
    std::uint32_t unk1 = 1;
    std::uint32_t node_index = 1;
    std::size_t node_ptr = 0;
    std::size_t entry_pos = 0;

    for (std::uint32_t level = 0; entry_pos < entries.size(); ++level) {
        std::size_t arr1_ptr = unk0;
        const std::size_t arr1_old_ptr = arr1_ptr;
        std::uint32_t group_count = 0;

        while (true) {
            const std::uint32_t current = entry_pos < entries.size() ? entries[entry_pos] : 0;
            if (level != (current >> 16)) {
                break;
            }
            auto &node = nodes[arr1[node_ptr]];
            node.has_children = false;
            node.look_behind = (current & 0x100u) != 0;
            node.value = static_cast<std::uint8_t>(current & 0xFFu);
            ++entry_pos;
            ++node_ptr;
            ++group_count;
        }

        const auto next_width = 2u * (unk1 - group_count);
        if (group_count < unk1) {
            unk1 -= group_count;
            for (std::uint32_t i = 0; i < unk1; ++i) {
                auto &node = nodes[arr1[node_ptr]];
                node.has_children = true;
                for (int branch = 0; branch < 2; ++branch) {
                    arr1[arr1_ptr++] = node.children[branch] = node_index++;
                }
                ++node_ptr;
            }
        }
        unk1 = next_width;
        node_ptr = arr1_old_ptr;
        unk0 ^= 0x200u;
    }

    return nodes;
}

std::vector<std::uint8_t> DecodeDscToCompiled(const std::vector<std::uint8_t> &input) {
    if (input.size() < kDscMagic.size() + 16 || !std::equal(kDscMagic.begin(), kDscMagic.begin() + 16, input.begin())) {
        throw std::runtime_error("Input file is not a DSC script.");
    }

    ByteReader reader(input);
    reader.Skip(16);
    std::uint32_t key = reader.ReadU32();
    const auto output_size = reader.ReadU32();
    reader.Skip(8);

    std::array<std::uint8_t, 512> lengths{};
    for (std::size_t i = 0; i < lengths.size(); ++i) {
        const auto encrypted = reader.ReadU8();
        lengths[i] = static_cast<std::uint8_t>(encrypted - NextKeyByte(key));
    }

    const auto nodes = BuildDscTree(lengths);
    const auto bitstream = reader.ReadBytes(reader.Remaining());
    BitReader bits(bitstream);

    std::vector<std::uint8_t> output;
    output.reserve(output_size);
    while (output.size() < output_size) {
        std::uint32_t node_index = 0;
        while (nodes[node_index].has_children) {
            node_index = nodes[node_index].children[bits.ReadBit() ? 1 : 0];
        }

        if (nodes[node_index].look_behind) {
            std::uint32_t offset = 0;
            for (int i = 0; i < 12; ++i) {
                offset = (offset << 1) | (bits.ReadBit() ? 1u : 0u);
            }
            std::size_t repetitions = static_cast<std::size_t>(nodes[node_index].value) + 2;
            if (offset + 2 > output.size()) {
                throw std::runtime_error("DSC look-behind offset is invalid.");
            }
            std::size_t look_behind = output.size() - offset - 2;
            while (repetitions-- && output.size() < output_size) {
                output.push_back(output[look_behind++]);
            }
        } else {
            output.push_back(nodes[node_index].value);
        }
    }

    return output;
}

std::array<std::uint8_t, kDscSymbolCount> BuildCodeLengths(const std::array<std::uint64_t, kDscSymbolCount> &frequencies) {
    std::array<std::uint8_t, kDscSymbolCount> lengths{};

    struct QueueItem {
        std::uint64_t frequency;
        std::uint64_t order;
        int index;
        bool operator>(const QueueItem &other) const {
            if (frequency != other.frequency) {
                return frequency > other.frequency;
            }
            return order > other.order;
        }
    };

    std::vector<HuffmanBuildNode> nodes;
    nodes.reserve(kDscSymbolCount * 2);
    std::priority_queue<QueueItem, std::vector<QueueItem>, std::greater<QueueItem>> queue;

    std::uint64_t order = 0;
    for (int symbol = 0; symbol < static_cast<int>(kDscSymbolCount); ++symbol) {
        if (frequencies[symbol] == 0) {
            continue;
        }
        HuffmanBuildNode node;
        node.frequency = frequencies[symbol];
        node.symbol = symbol;
        node.order = order++;
        nodes.push_back(node);
        queue.push({node.frequency, node.order, static_cast<int>(nodes.size() - 1)});
    }

    if (queue.empty()) {
        throw std::runtime_error("Cannot encode an empty compiled script.");
    }

    if (queue.size() == 1) {
        const auto only = queue.top();
        lengths[static_cast<std::size_t>(nodes[only.index].symbol)] = 1;
        return lengths;
    }

    while (queue.size() > 1) {
        const auto left = queue.top();
        queue.pop();
        const auto right = queue.top();
        queue.pop();

        HuffmanBuildNode parent;
        parent.frequency = left.frequency + right.frequency;
        parent.left = left.index;
        parent.right = right.index;
        parent.order = order++;
        nodes.push_back(parent);
        queue.push({parent.frequency, parent.order, static_cast<int>(nodes.size() - 1)});
    }

    const int root = queue.top().index;
    const auto assign_depths = [&](const auto &self, int node_index, std::uint8_t depth) -> void {
        const auto &node = nodes[static_cast<std::size_t>(node_index)];
        if (node.symbol >= 0) {
            lengths[static_cast<std::size_t>(node.symbol)] = depth == 0 ? 1 : depth;
            return;
        }
        self(self, node.left, static_cast<std::uint8_t>(depth + 1));
        self(self, node.right, static_cast<std::uint8_t>(depth + 1));
    };
    assign_depths(assign_depths, root, 0);
    return lengths;
}

std::uint32_t HashSequence(const std::vector<std::uint8_t> &data, std::size_t position) {
    return ((static_cast<std::uint32_t>(data[position]) << 16)
        ^ (static_cast<std::uint32_t>(data[position + 1]) << 8)
        ^ static_cast<std::uint32_t>(data[position + 2]))
        & static_cast<std::uint32_t>(kMatchHashSize - 1);
}

std::vector<MatchInfo> FindBestMatches(const std::vector<std::uint8_t> &compiled) {
    std::vector<MatchInfo> matches(compiled.size());
    if (compiled.size() < kDscMinMatchLength) {
        return matches;
    }

    std::vector<int> head(kMatchHashSize, -1);
    std::vector<int> previous(compiled.size(), -1);

    for (std::size_t position = 0; position < compiled.size(); ++position) {
        if (position + kDscMinMatchLength <= compiled.size()) {
            const auto hash = HashSequence(compiled, position);
            int candidate = head[hash];
            int checks = 0;
            MatchInfo best;

            while (candidate >= 0 && checks < kMaxHashChainChecks) {
                const auto candidate_pos = static_cast<std::size_t>(candidate);
                const auto distance = position - candidate_pos;
                if (distance < 2) {
                    candidate = previous[candidate_pos];
                    ++checks;
                    continue;
                }
                if (distance > kDscMaxDistance) {
                    break;
                }

                std::size_t length = 0;
                const auto max_length = std::min<std::size_t>(kDscMaxMatchLength, compiled.size() - position);
                while (length < max_length && compiled[candidate_pos + length] == compiled[position + length]) {
                    ++length;
                }

                if (length >= kDscMinMatchLength && length > best.length) {
                    best.length = static_cast<std::uint16_t>(length);
                    best.distance = static_cast<std::uint16_t>(distance);
                    if (length == kDscMaxMatchLength) {
                        break;
                    }
                }

                candidate = previous[candidate_pos];
                ++checks;
            }

            matches[position] = best;
            previous[position] = head[hash];
            head[hash] = static_cast<int>(position);
        }
    }

    return matches;
}

std::vector<EncodedToken> TokenizeCompiled(
    const std::vector<std::uint8_t> &compiled,
    const std::vector<MatchInfo> &matches,
    const std::array<int, kDscSymbolCount> &estimated_bits) {
    constexpr int kLargeCost = (std::numeric_limits<int>::max)() / 4;

    std::vector<int> best_cost(compiled.size() + 1, kLargeCost);
    std::vector<std::uint16_t> best_length(compiled.size(), 1);
    std::vector<std::uint16_t> best_distance(compiled.size(), 0);
    best_cost[compiled.size()] = 0;

    for (std::size_t reverse = compiled.size(); reverse-- > 0;) {
        int cost = estimated_bits[compiled[reverse]] + best_cost[reverse + 1];
        best_length[reverse] = 1;
        best_distance[reverse] = 0;

        const auto &match = matches[reverse];
        if (match.length >= kDscMinMatchLength) {
            for (std::size_t length = kDscMinMatchLength; length <= match.length; ++length) {
                const auto symbol = static_cast<std::size_t>(0x100 + length - 2);
                const int candidate_cost = estimated_bits[symbol] + 12 + best_cost[reverse + length];
                if (candidate_cost < cost) {
                    cost = candidate_cost;
                    best_length[reverse] = static_cast<std::uint16_t>(length);
                    best_distance[reverse] = match.distance;
                }
            }
        }

        best_cost[reverse] = cost;
    }

    std::vector<EncodedToken> tokens;
    for (std::size_t position = 0; position < compiled.size();) {
        if (best_length[position] > 1) {
            const auto length = best_length[position];
            tokens.push_back({
                static_cast<std::uint16_t>(0x100 + length - 2),
                best_distance[position],
                length,
            });
            position += length;
        } else {
            tokens.push_back({compiled[position], 0, 1});
            ++position;
        }
    }
    return tokens;
}

std::array<std::uint64_t, kDscSymbolCount> CountTokenFrequencies(const std::vector<EncodedToken> &tokens) {
    std::array<std::uint64_t, kDscSymbolCount> frequencies{};
    for (const auto &token : tokens) {
        ++frequencies[token.symbol];
    }
    return frequencies;
}

std::array<int, kDscSymbolCount> BuildEstimatedBitCosts(const std::array<std::uint8_t, kDscSymbolCount> &lengths) {
    std::array<int, kDscSymbolCount> estimated{};
    estimated.fill(9);
    for (std::size_t i = 0; i < lengths.size(); ++i) {
        if (lengths[i] != 0) {
            estimated[i] = lengths[i];
        }
    }
    return estimated;
}

std::vector<EncodedToken> OptimizeTokens(const std::vector<std::uint8_t> &compiled) {
    auto matches = FindBestMatches(compiled);
    std::array<int, kDscSymbolCount> estimated_bits{};
    estimated_bits.fill(9);

    std::vector<EncodedToken> tokens;
    for (int iteration = 0; iteration < 4; ++iteration) {
        auto candidate_tokens = TokenizeCompiled(compiled, matches, estimated_bits);
        const auto frequencies = CountTokenFrequencies(candidate_tokens);
        const auto lengths = BuildCodeLengths(frequencies);
        auto next_estimated = estimated_bits;
        for (std::size_t i = 0; i < lengths.size(); ++i) {
            if (lengths[i] != 0) {
                next_estimated[i] = lengths[i];
            }
        }
        tokens = std::move(candidate_tokens);
        if (next_estimated == estimated_bits) {
            break;
        }
        estimated_bits = next_estimated;
    }
    return tokens;
}

std::array<DscBitCode, 512> BuildCodesFromTree(const std::array<DscNode, 1024> &nodes) {
    std::array<DscBitCode, 512> codes{};
    std::vector<bool> path;
    const auto visit = [&](const auto &self, std::uint32_t node_index) -> void {
        const auto &node = nodes[node_index];
        if (!node.has_children) {
            const std::size_t symbol = (node.look_behind ? 0x100u : 0u) | node.value;
            codes[symbol].bits = path;
            if (codes[symbol].bits.empty()) {
                codes[symbol].bits.push_back(false);
            }
            return;
        }
        path.push_back(false);
        self(self, node.children[0]);
        path.back() = true;
        self(self, node.children[1]);
        path.pop_back();
    };
    visit(visit, 0);
    return codes;
}

std::vector<std::uint8_t> EncodeCompiledToDsc(const std::vector<std::uint8_t> &compiled) {
    const auto tokens = OptimizeTokens(compiled);
    const auto frequencies = CountTokenFrequencies(tokens);
    const auto lengths = BuildCodeLengths(frequencies);
    const auto nodes = BuildDscTree(lengths);
    const auto codes = BuildCodesFromTree(nodes);

    BitWriter writer;
    for (const auto &token : tokens) {
        writer.WriteBits(codes[token.symbol].bits);
        if (token.length > 1) {
            writer.WriteValue(static_cast<std::uint32_t>(token.distance - 2), 12);
        }
    }
    const auto bitstream = writer.Finish();

    std::vector<std::uint8_t> output;
    output.reserve(16 + 16 + 512 + bitstream.size());
    output.insert(output.end(), kDscMagic.begin(), kDscMagic.begin() + 16);
    constexpr std::uint32_t key_seed = 0;
    AppendU32(output, key_seed);
    AppendU32(output, static_cast<std::uint32_t>(compiled.size()));
    AppendU32(output, 0);
    AppendU32(output, 0);

    std::uint32_t key = key_seed;
    for (const auto length : lengths) {
        output.push_back(static_cast<std::uint8_t>(length + NextKeyByte(key)));
    }
    output.insert(output.end(), bitstream.begin(), bitstream.end());
    return output;
}

ScriptConfig GetScriptConfig(const std::vector<std::uint8_t> &compiled) {
    if (StartsWith(compiled, kCompiledMagic)) {
        return GetVer100Config();
    }
    return GetVer000Config();
}

std::size_t FindTextBoundary(const std::vector<std::uint8_t> &compiled) {
    std::size_t found = std::string::npos;
    for (std::size_t pos = 0; pos + 4 <= compiled.size(); ++pos) {
        if (compiled[pos] == 0x1B && compiled[pos + 1] == 0x00 && compiled[pos + 2] == 0x00 && compiled[pos + 3] == 0x00) {
            found = pos;
        }
    }
    if (found == std::string::npos) {
        return compiled.size();
    }
    return found + 4;
}

std::vector<TextSlot> ParseTextSlots(const std::vector<std::uint8_t> &text_bytes) {
    std::vector<TextSlot> slots;
    if (text_bytes.empty() || (text_bytes.size() == 1 && text_bytes[0] == 0)) {
        return slots;
    }

    std::size_t position = 0;
    while (position < text_bytes.size()) {
        auto end = position;
        while (end < text_bytes.size() && text_bytes[end] != 0) {
            ++end;
        }
        TextSlot slot;
        slot.offset = static_cast<std::uint32_t>(position);
        slot.bytes.assign(text_bytes.begin() + static_cast<std::ptrdiff_t>(position), text_bytes.begin() + static_cast<std::ptrdiff_t>(end));
        slots.push_back(std::move(slot));
        if (end == text_bytes.size()) {
            break;
        }
        position = end + 1;
        if (position == text_bytes.size()) {
            break;
        }
    }
    return slots;
}

ScriptLayout LoadScriptLayout(const std::filesystem::path &path) {
    ScriptLayout layout;
    const auto raw_bytes = ReadAllBytes(path);
    if (raw_bytes.size() >= 16 && std::equal(kDscMagic.begin(), kDscMagic.begin() + 16, raw_bytes.begin())) {
        layout.container_kind = ScriptContainerKind::DscCompressed;
        layout.compiled_data = DecodeDscToCompiled(raw_bytes);
    } else {
        layout.container_kind = ScriptContainerKind::RawCompiled;
        layout.compiled_data = raw_bytes;
    }

    layout.config = GetScriptConfig(layout.compiled_data);
    std::size_t header_size = layout.config.header_size;
    if (layout.config.additional_header_pos.has_value()) {
        header_size += ReadU32(layout.compiled_data, layout.config.additional_header_pos.value());
    }
    const auto text_boundary = FindTextBoundary(layout.compiled_data);
    if (header_size > text_boundary || text_boundary > layout.compiled_data.size()) {
        throw std::runtime_error("Script section boundaries are invalid.");
    }

    layout.header_bytes.assign(layout.compiled_data.begin(), layout.compiled_data.begin() + static_cast<std::ptrdiff_t>(header_size));
    layout.code_bytes.assign(layout.compiled_data.begin() + static_cast<std::ptrdiff_t>(header_size), layout.compiled_data.begin() + static_cast<std::ptrdiff_t>(text_boundary));
    const std::vector<std::uint8_t> text_bytes(layout.compiled_data.begin() + static_cast<std::ptrdiff_t>(text_boundary), layout.compiled_data.end());
    layout.text_slots = ParseTextSlots(text_bytes);
    return layout;
}

bool CheckFunction(const std::vector<std::uint8_t> &code, std::size_t pos, std::uint32_t function_id, std::int32_t relative_offset) {
    if (relative_offset < 0) {
        if (static_cast<std::size_t>(-relative_offset) > pos) {
            return false;
        }
    }
    const auto target = static_cast<std::ptrdiff_t>(pos) + relative_offset;
    if (target < 0 || static_cast<std::size_t>(target + 4) > code.size()) {
        return false;
    }
    return ReadU32(code, static_cast<std::size_t>(target)) == function_id;
}

int KindRank(TextKind kind) {
    switch (kind) {
    case TextKind::Name:
        return 7;
    case TextKind::Text:
        return 6;
    case TextKind::RubyKanji:
        return 5;
    case TextKind::RubyFurigana:
        return 4;
    case TextKind::Backlog:
        return 3;
    case TextKind::File:
        return 2;
    case TextKind::Other:
    default:
        return 1;
    }
}

std::pair<TextKind, std::wstring> ClassifyStringReference(
    const std::vector<std::uint8_t> &code,
    std::size_t pos,
    const ScriptConfig &config,
    const std::unordered_map<std::uint32_t, TextSlot> &text_slots,
    std::uint32_t decode_codepage) {
    const auto opcode = ReadU32(code, pos - 4);
    if (opcode == config.file_type) {
        return {TextKind::File, L"FILE"};
    }
    if (opcode != config.string_type) {
        return {TextKind::Other, L"OTHER"};
    }

    if (CheckFunction(code, pos, config.text_function, config.name_pos)) {
        return {TextKind::Name, L"NAME"};
    }
    if (CheckFunction(code, pos, config.text_function, config.text_pos)) {
        std::wstring comment = L"TEXT";
        const auto delta = config.text_pos - config.name_pos;
        const auto name_pointer_pos = static_cast<std::ptrdiff_t>(pos) + delta;
        if (name_pointer_pos >= 0 && static_cast<std::size_t>(name_pointer_pos + 4) <= code.size()) {
            const auto name_dword = ReadU32(code, static_cast<std::size_t>(name_pointer_pos));
            if (name_dword != 0) {
                const auto name_offset = name_dword - static_cast<std::uint32_t>(code.size());
                const auto it = text_slots.find(name_offset);
                if (it != text_slots.end()) {
                    comment += L" 【" + DecodeScriptBytes(it->second.bytes, decode_codepage) + L"】";
                }
            }
        }
        return {TextKind::Text, comment};
    }
    if (CheckFunction(code, pos, config.ruby_function, config.ruby_kanji_pos)) {
        return {TextKind::RubyKanji, L"TEXT RUBY KANJI"};
    }
    if (CheckFunction(code, pos, config.ruby_function, config.ruby_furigana_pos)) {
        return {TextKind::RubyFurigana, L"TEXT RUBY FURIGANA"};
    }
    if (CheckFunction(code, pos, config.backlog_function, config.backlog_pos)) {
        return {TextKind::Backlog, L"TEXT BACKLOG"};
    }
    return {TextKind::Other, L"OTHER"};
}

std::unordered_map<std::uint32_t, TextSlot> ToTextSlotMap(const std::vector<TextSlot> &slots) {
    std::unordered_map<std::uint32_t, TextSlot> map;
    for (const auto &slot : slots) {
        map.emplace(slot.offset, slot);
    }
    return map;
}

std::vector<TextEntry> ExtractEntries(const ScriptLayout &layout, std::uint32_t decode_codepage) {
    const auto text_slot_map = ToTextSlotMap(layout.text_slots);
    std::map<std::uint32_t, ExtractedEntryInfo> info_by_offset;

    for (std::size_t pos = 4; pos + 4 <= layout.code_bytes.size(); pos += 4) {
        const auto value = ReadU32(layout.code_bytes, pos);
        if (value < layout.code_bytes.size()) {
            continue;
        }
        const auto text_offset = value - static_cast<std::uint32_t>(layout.code_bytes.size());
        const auto slot_it = text_slot_map.find(text_offset);
        if (slot_it == text_slot_map.end()) {
            continue;
        }

        auto [kind, comment] = ClassifyStringReference(layout.code_bytes, pos, layout.config, text_slot_map, decode_codepage);
        auto &info = info_by_offset[text_offset];
        if (info.comment.empty() || KindRank(kind) > KindRank(info.kind)) {
            info.kind = kind;
            info.comment = std::move(comment);
        }
        info.code_offsets.push_back(static_cast<std::uint32_t>(pos));
    }

    std::vector<TextEntry> entries;
    entries.reserve(info_by_offset.size());
    std::uint32_t index = 1;
    for (const auto &[offset, info] : info_by_offset) {
        const auto slot_it = text_slot_map.find(offset);
        if (slot_it == text_slot_map.end()) {
            continue;
        }
        TextEntry entry;
        entry.index = index++;
        entry.text_offset = offset;
        entry.kind = info.kind;
        entry.comment = info.comment;
        entry.code_offsets = info.code_offsets;
        entry.original_bytes = slot_it->second.bytes;
        entry.original_text = DecodeScriptBytes(slot_it->second.bytes, decode_codepage);
        entries.push_back(std::move(entry));
    }
    return entries;
}

std::wstring ContainerToString(ScriptContainerKind kind) {
    return kind == ScriptContainerKind::DscCompressed ? L"dsc" : L"compiled";
}

ScriptContainerKind ParseContainer(const std::wstring &value) {
    if (value == L"compiled") {
        return ScriptContainerKind::RawCompiled;
    }
    return ScriptContainerKind::DscCompressed;
}

TextKind ParseKind(const std::wstring &value) {
    if (value == L"name") return TextKind::Name;
    if (value == L"text") return TextKind::Text;
    if (value == L"ruby_kanji") return TextKind::RubyKanji;
    if (value == L"ruby_furigana") return TextKind::RubyFurigana;
    if (value == L"backlog") return TextKind::Backlog;
    if (value == L"file") return TextKind::File;
    return TextKind::Other;
}

std::wstring MakeProjectText(const TextProject &project, const std::filesystem::path &script_path) {
    std::wostringstream stream;
    stream << kProjectMagic << L"\n";
    stream << L"# script_name=" << script_path.filename().wstring() << L"\n";
    stream << L"# container=" << ContainerToString(project.container_kind) << L"\n";
    stream << L"# decode_cp=" << project.decode_codepage << L"\n";
    stream << L"# encode_cp=" << project.encode_codepage << L"\n\n";

    for (const auto &entry : project.entries) {
        stream << L"[ENTRY]\n";
        stream << L"id=" << std::setw(4) << std::setfill(L'0') << entry.index << L"\n";
        stream << L"kind=" << ToString(entry.kind) << L"\n";
        stream << L"text_offset=" << std::uppercase << std::hex << std::setw(8) << std::setfill(L'0') << entry.text_offset << std::dec << L"\n";
        stream << L"code_offsets=" << JoinHexOffsets(entry.code_offsets) << L"\n";
        stream << L"comment=" << EscapeProjectText(entry.comment) << L"\n";
        stream << L"src=" << EscapeProjectText(entry.original_text) << L"\n";
        stream << L"dst=" << EscapeProjectText(entry.translation_text) << L"\n\n";
    }
    return stream.str();
}

TextProject ParseProjectText(const std::wstring &content) {
    const auto lines = SplitLines(content);
    if (lines.empty() || Trim(lines[0]) != kProjectMagic) {
        throw std::runtime_error("This file is not a BGI_Hazuki text project.");
    }

    TextProject project;
    std::optional<TextEntry> current_entry;

    for (std::size_t i = 1; i < lines.size(); ++i) {
        const auto line = Trim(lines[i]);
        if (line.empty()) {
            if (current_entry.has_value()) {
                project.entries.push_back(*current_entry);
                current_entry.reset();
            }
            continue;
        }
        if (line[0] == L'#') {
            const auto separator = line.find(L'=');
            if (separator == std::wstring::npos) {
                continue;
            }
            const auto key = Trim(line.substr(1, separator - 1));
            const auto value = Trim(line.substr(separator + 1));
            if (key == L"container") {
                project.container_kind = ParseContainer(value);
            } else if (key == L"decode_cp") {
                project.decode_codepage = static_cast<std::uint32_t>(std::stoul(value));
            } else if (key == L"encode_cp") {
                project.encode_codepage = static_cast<std::uint32_t>(std::stoul(value));
            }
            continue;
        }
        if (line == L"[ENTRY]") {
            if (current_entry.has_value()) {
                project.entries.push_back(*current_entry);
            }
            current_entry = TextEntry{};
            continue;
        }
        if (!current_entry.has_value()) {
            continue;
        }
        const auto separator = line.find(L'=');
        if (separator == std::wstring::npos) {
            continue;
        }
        const auto key = Trim(line.substr(0, separator));
        const auto value = line.substr(separator + 1);
        if (key == L"id") {
            current_entry->index = static_cast<std::uint32_t>(std::stoul(value));
        } else if (key == L"kind") {
            current_entry->kind = ParseKind(value);
        } else if (key == L"text_offset") {
            current_entry->text_offset = static_cast<std::uint32_t>(std::stoul(value, nullptr, 16));
        } else if (key == L"code_offsets") {
            current_entry->code_offsets = ParseHexOffsets(value);
        } else if (key == L"comment") {
            current_entry->comment = UnescapeProjectText(value);
        } else if (key == L"src") {
            current_entry->original_text = UnescapeProjectText(value);
        } else if (key == L"dst") {
            current_entry->translation_text = UnescapeProjectText(value);
        }
    }

    if (current_entry.has_value()) {
        project.entries.push_back(*current_entry);
    }

    return project;
}

std::vector<std::uint8_t> RebuildCompiledScript(
    const ScriptLayout &layout,
    const TextProject &project,
    std::uint32_t encode_codepage) {
    const auto slot_map = ToTextSlotMap(layout.text_slots);
    std::unordered_map<std::uint32_t, std::vector<std::uint8_t>> replacement_bytes;
    for (const auto &entry : project.entries) {
        const auto text = entry.translation_text.empty() ? entry.original_text : entry.translation_text;
        replacement_bytes[entry.text_offset] = EncodeScriptText(text, encode_codepage);
    }

    std::vector<std::uint8_t> new_text_bytes;
    std::unordered_map<std::uint32_t, std::uint32_t> new_offsets;
    for (const auto &slot : layout.text_slots) {
        new_offsets[slot.offset] = static_cast<std::uint32_t>(new_text_bytes.size());
        const auto it = replacement_bytes.find(slot.offset);
        const auto &bytes = it != replacement_bytes.end() ? it->second : slot.bytes;
        new_text_bytes.insert(new_text_bytes.end(), bytes.begin(), bytes.end());
        new_text_bytes.push_back(0);
    }

    auto new_code_bytes = layout.code_bytes;
    for (std::size_t pos = 4; pos + 4 <= new_code_bytes.size(); pos += 4) {
        const auto value = ReadU32(new_code_bytes, pos);
        if (value < layout.code_bytes.size()) {
            continue;
        }
        const auto old_text_offset = value - static_cast<std::uint32_t>(layout.code_bytes.size());
        const auto offset_it = new_offsets.find(old_text_offset);
        if (offset_it == new_offsets.end()) {
            continue;
        }
        WriteU32(new_code_bytes, pos, static_cast<std::uint32_t>(layout.code_bytes.size()) + offset_it->second);
    }

    std::vector<std::uint8_t> compiled;
    compiled.reserve(layout.header_bytes.size() + new_code_bytes.size() + new_text_bytes.size());
    compiled.insert(compiled.end(), layout.header_bytes.begin(), layout.header_bytes.end());
    compiled.insert(compiled.end(), new_code_bytes.begin(), new_code_bytes.end());
    compiled.insert(compiled.end(), new_text_bytes.begin(), new_text_bytes.end());
    return compiled;
}

}  // namespace

bool IsDscScript(const std::filesystem::path &path) {
    if (!std::filesystem::is_regular_file(path)) {
        return false;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    std::array<std::uint8_t, 16> buffer{};
    file.read(reinterpret_cast<char *>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
    return file.gcount() == static_cast<std::streamsize>(buffer.size())
        && std::equal(kDscMagic.begin(), kDscMagic.begin() + 16, buffer.begin());
}

bool IsCompiledScript(const std::filesystem::path &path) {
    if (!std::filesystem::is_regular_file(path)) {
        return false;
    }
    const auto bytes = ReadAllBytes(path);
    return StartsWith(bytes, kCompiledMagic);
}

std::filesystem::path GetDefaultProjectPath(const std::filesystem::path &script_path) {
    return std::filesystem::path(script_path.wstring() + kProjectSuffix);
}

std::filesystem::path InferScriptPathFromProject(const std::filesystem::path &project_path) {
    const auto project_name = project_path.wstring();
    if (!EndsWithInsensitive(project_name, kProjectSuffix)) {
        throw std::runtime_error("The project file name must end with .hazuki.txt.");
    }
    return std::filesystem::path(project_name.substr(0, project_name.size() - std::wcslen(kProjectSuffix)));
}

std::filesystem::path GetDefaultPatchedPath(const std::filesystem::path &script_path) {
    return std::filesystem::path(script_path.wstring() + L".patched");
}

TextProject ExtractTextProject(
    const std::filesystem::path &script_path,
    std::uint32_t decode_codepage,
    std::uint32_t encode_codepage) {
    auto layout = LoadScriptLayout(script_path);
    TextProject project;
    project.container_kind = layout.container_kind;
    project.decode_codepage = decode_codepage;
    project.encode_codepage = encode_codepage;
    project.entries = ExtractEntries(layout, decode_codepage);
    return project;
}

void SaveTextProject(
    const TextProject &project,
    const std::filesystem::path &script_path,
    const std::filesystem::path &output_path) {
    const auto wide_text = MakeProjectText(project, script_path);
    const auto utf8 = WideToUtf8(wide_text);
    std::vector<std::uint8_t> bytes = {0xEF, 0xBB, 0xBF};
    bytes.insert(bytes.end(), utf8.begin(), utf8.end());
    WriteAllBytes(output_path, bytes);
}

TextProject LoadTextProject(const std::filesystem::path &project_path) {
    auto bytes = ReadAllBytes(project_path);
    if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
        bytes.erase(bytes.begin(), bytes.begin() + 3);
    }
    const std::string utf8(bytes.begin(), bytes.end());
    return ParseProjectText(Utf8ToWide(utf8));
}

void ApplyTextProject(
    const std::filesystem::path &project_path,
    const std::filesystem::path &script_path,
    const std::filesystem::path &output_path,
    std::uint32_t fallback_encode_codepage) {
    auto project = LoadTextProject(project_path);
    auto layout = LoadScriptLayout(script_path);
    const auto encode_codepage = fallback_encode_codepage == 0 ? project.encode_codepage : fallback_encode_codepage;
    const auto compiled = RebuildCompiledScript(layout, project, encode_codepage);
    if (layout.container_kind == ScriptContainerKind::DscCompressed) {
        WriteAllBytes(output_path, EncodeCompiledToDsc(compiled));
    } else {
        WriteAllBytes(output_path, compiled);
    }
}

const wchar_t *ToString(TextKind kind) {
    switch (kind) {
    case TextKind::Name:
        return L"name";
    case TextKind::Text:
        return L"text";
    case TextKind::RubyKanji:
        return L"ruby_kanji";
    case TextKind::RubyFurigana:
        return L"ruby_furigana";
    case TextKind::Backlog:
        return L"backlog";
    case TextKind::File:
        return L"file";
    case TextKind::Other:
    default:
        return L"other";
    }
}

}  // namespace hazuki::dsc
