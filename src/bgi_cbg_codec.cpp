#include "bgi_cbg_codec.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace hazuki {
namespace {

constexpr std::size_t kMagicSize = 16;
constexpr std::array<std::uint8_t, kMagicSize> kMagic = {
    0x43, 0x6F, 0x6D, 0x70, 0x72, 0x65, 0x73, 0x73,
    0x65, 0x64, 0x42, 0x47, 0x5F, 0x5F, 0x5F, 0x00,
};

struct ByteReader {
    explicit ByteReader(std::vector<std::uint8_t> bytes) : data(std::move(bytes)) {}

    std::uint8_t ReadU8() {
        EnsureAvailable(1);
        return data[position++];
    }

    std::uint16_t ReadU16() {
        EnsureAvailable(2);
        const auto value = static_cast<std::uint16_t>(data[position])
            | (static_cast<std::uint16_t>(data[position + 1]) << 8);
        position += 2;
        return value;
    }

    std::uint32_t ReadU32() {
        EnsureAvailable(4);
        const auto value = static_cast<std::uint32_t>(data[position])
            | (static_cast<std::uint32_t>(data[position + 1]) << 8)
            | (static_cast<std::uint32_t>(data[position + 2]) << 16)
            | (static_cast<std::uint32_t>(data[position + 3]) << 24);
        position += 4;
        return value;
    }

    std::vector<std::uint8_t> ReadBytes(std::size_t count) {
        EnsureAvailable(count);
        std::vector<std::uint8_t> chunk(data.begin() + static_cast<std::ptrdiff_t>(position), data.begin() + static_cast<std::ptrdiff_t>(position + count));
        position += count;
        return chunk;
    }

    void Skip(std::size_t count) {
        EnsureAvailable(count);
        position += count;
    }

    std::size_t Remaining() const {
        return data.size() - position;
    }

    std::size_t Position() const {
        return position;
    }

    void Seek(std::size_t new_position) {
        if (new_position > data.size()) {
            throw std::runtime_error("Attempted to seek beyond the end of the buffer.");
        }
        position = new_position;
    }

    const std::vector<std::uint8_t> data;
    std::size_t position = 0;

private:
    void EnsureAvailable(std::size_t count) const {
        if (position + count > data.size()) {
            throw std::runtime_error("Unexpected end of CBG data.");
        }
    }
};

struct HuffmanNode {
    bool is_leaf = false;
    std::uint32_t weight = 0;
    int value = -1;
    int left = -1;
    int right = -1;
    int order = -1;
};

struct HuffmanCode {
    std::uint32_t bits = 0;
    int bit_count = 0;
    std::uint32_t weight = 0;
};

struct HuffmanTree {
    std::vector<HuffmanNode> nodes;
    std::array<HuffmanCode, 256> codes{};
    int root = -1;
};

class BitReader {
public:
    explicit BitReader(const std::vector<std::uint8_t> &data) : data_(data) {}

    bool ReadBit() {
        if (byte_index_ >= data_.size()) {
            throw std::runtime_error("Unexpected end of Huffman bitstream.");
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
    void Write(std::uint32_t value, int bit_count) {
        while (bit_count > 0) {
            const int chunk = std::min(free_bits_, bit_count);
            const std::uint32_t mask = static_cast<std::uint32_t>((1u << chunk) - 1u);
            bit_count -= chunk;
            free_bits_ -= chunk;
            current_byte_ |= static_cast<std::uint8_t>(((value >> bit_count) & mask) << free_bits_);
            if (free_bits_ == 0) {
                FlushByte();
            }
        }
    }

    std::vector<std::uint8_t> Finish() {
        if (free_bits_ != 8) {
            FlushByte();
        }
        return std::move(bytes_);
    }

private:
    void FlushByte() {
        bytes_.push_back(current_byte_);
        current_byte_ = 0;
        free_bits_ = 8;
    }

    std::vector<std::uint8_t> bytes_;
    std::uint8_t current_byte_ = 0;
    int free_bits_ = 8;
};

std::vector<std::uint8_t> ReadAllBytes(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Failed to open input file.");
    }
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

void WriteAllBytes(const std::filesystem::path &path, const std::vector<std::uint8_t> &data) {
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Failed to create output file.");
    }
    file.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!file) {
        throw std::runtime_error("Failed to write output file.");
    }
}

bool StartsWithMagic(const std::vector<std::uint8_t> &data) {
    return data.size() >= kMagicSize && std::equal(kMagic.begin(), kMagic.end(), data.begin());
}

std::uint8_t NextKeyByte(std::uint32_t &key) {
    const std::uint32_t v0 = 20021u * (key & 0xFFFFu);
    std::uint32_t v1 = key >> 16;
    v1 = v1 * 20021u + key * 346u;
    v1 = (v1 + (v0 >> 16)) & 0xFFFFu;
    key = (v1 << 16) + (v0 & 0xFFFFu) + 1u;
    return static_cast<std::uint8_t>(v1 & 0xFFu);
}

std::uint32_t ReadVariableUInt(ByteReader &reader) {
    std::uint32_t result = 0;
    std::uint32_t shift = 0;
    while (true) {
        const auto current = reader.ReadU8();
        result |= static_cast<std::uint32_t>(current & 0x7Fu) << shift;
        if ((current & 0x80u) == 0) {
            return result;
        }
        shift += 7;
        if (shift > 28) {
            throw std::runtime_error("Variable-length integer is too large.");
        }
    }
}

void WriteVariableUInt(std::vector<std::uint8_t> &output, std::uint32_t value) {
    do {
        std::uint8_t byte = static_cast<std::uint8_t>(value & 0x7Fu);
        value >>= 7;
        if (value != 0) {
            byte |= 0x80u;
        }
        output.push_back(byte);
    } while (value != 0);
}

std::vector<std::uint8_t> DecryptWeightTable(ByteReader &reader) {
    std::uint32_t key = reader.ReadU32();
    const auto data_size = reader.ReadU32();
    const auto expected_sum = reader.ReadU8();
    const auto expected_xor = reader.ReadU8();
    reader.Skip(2);
    auto encrypted = reader.ReadBytes(data_size);

    std::uint8_t actual_sum = 0;
    std::uint8_t actual_xor = 0;
    for (auto &byte : encrypted) {
        byte = static_cast<std::uint8_t>(byte - NextKeyByte(key));
        actual_sum = static_cast<std::uint8_t>(actual_sum + byte);
        actual_xor = static_cast<std::uint8_t>(actual_xor ^ byte);
    }

    if (actual_sum != expected_sum || actual_xor != expected_xor) {
        throw std::runtime_error("CBG weight-table checksum mismatch.");
    }

    return encrypted;
}

HuffmanTree BuildHuffmanTree(const std::array<std::uint32_t, 256> &frequencies) {
    HuffmanTree tree;
    std::vector<int> active;
    tree.nodes.reserve(512);

    for (int symbol = 0; symbol < 256; ++symbol) {
        if (frequencies[symbol] == 0) {
            continue;
        }
        HuffmanNode node;
        node.is_leaf = true;
        node.weight = frequencies[symbol];
        node.value = symbol;
        node.order = symbol;
        tree.nodes.push_back(node);
        active.push_back(static_cast<int>(tree.nodes.size() - 1));
    }

    if (active.empty()) {
        throw std::runtime_error("Cannot build a Huffman tree from empty data.");
    }

    int next_order = 256;
    auto compare = [&tree](int left, int right) {
        const auto &a = tree.nodes[static_cast<std::size_t>(left)];
        const auto &b = tree.nodes[static_cast<std::size_t>(right)];
        if (a.weight != b.weight) {
            return a.weight < b.weight;
        }
        return a.order < b.order;
    };

    while (active.size() > 1) {
        std::sort(active.begin(), active.end(), compare);
        const int left = active[0];
        const int right = active[1];
        active.erase(active.begin(), active.begin() + 2);

        HuffmanNode parent;
        parent.is_leaf = false;
        parent.weight = tree.nodes[static_cast<std::size_t>(left)].weight + tree.nodes[static_cast<std::size_t>(right)].weight;
        parent.left = left;
        parent.right = right;
        parent.order = next_order++;
        tree.nodes.push_back(parent);
        active.push_back(static_cast<int>(tree.nodes.size() - 1));
    }

    tree.root = active.front();

    const auto assign_codes = [&](const auto &self, int node_index, std::uint32_t bits, int bit_count) -> void {
        auto &node = tree.nodes[static_cast<std::size_t>(node_index)];
        if (node.is_leaf) {
            auto &code = tree.codes[static_cast<std::size_t>(node.value)];
            code.bits = bits;
            code.bit_count = bit_count;
            code.weight = node.weight;
            return;
        }
        self(self, node.left, bits << 1, bit_count + 1);
        self(self, node.right, (bits << 1) | 1u, bit_count + 1);
    };

    assign_codes(assign_codes, tree.root, 0, 0);
    return tree;
}

std::vector<std::uint8_t> HuffmanDecode(const std::vector<std::uint8_t> &bitstream, std::size_t output_size, const HuffmanTree &tree) {
    std::vector<std::uint8_t> output;
    output.reserve(output_size);

    const auto &root = tree.nodes[static_cast<std::size_t>(tree.root)];
    if (root.is_leaf) {
        output.assign(output_size, static_cast<std::uint8_t>(root.value));
        return output;
    }

    BitReader reader(bitstream);
    while (output.size() < output_size) {
        int node_index = tree.root;
        while (!tree.nodes[static_cast<std::size_t>(node_index)].is_leaf) {
            const bool bit = reader.ReadBit();
            node_index = bit ? tree.nodes[static_cast<std::size_t>(node_index)].right : tree.nodes[static_cast<std::size_t>(node_index)].left;
            if (node_index < 0) {
                throw std::runtime_error("Encountered an invalid Huffman branch.");
            }
        }
        output.push_back(static_cast<std::uint8_t>(tree.nodes[static_cast<std::size_t>(node_index)].value));
    }

    return output;
}

std::vector<std::uint8_t> HuffmanEncode(const std::vector<std::uint8_t> &input, const HuffmanTree &tree) {
    BitWriter writer;
    for (const auto value : input) {
        const auto &code = tree.codes[static_cast<std::size_t>(value)];
        writer.Write(code.bits, code.bit_count);
    }
    return writer.Finish();
}

std::vector<std::uint8_t> DecodeRle(const std::vector<std::uint8_t> &input, std::size_t output_size) {
    ByteReader reader(input);
    std::vector<std::uint8_t> output;
    output.reserve(output_size);
    bool zero_flag = false;

    while (reader.Remaining() > 0 && output.size() < output_size) {
        auto count = static_cast<std::size_t>(ReadVariableUInt(reader));
        const auto remaining = output_size - output.size();
        count = std::min(count, remaining);

        if (zero_flag) {
            output.insert(output.end(), count, 0);
        } else {
            auto bytes = reader.ReadBytes(count);
            output.insert(output.end(), bytes.begin(), bytes.end());
        }
        zero_flag = !zero_flag;
    }

    if (output.size() != output_size) {
        throw std::runtime_error("Decoded RLE payload has an unexpected size.");
    }

    return output;
}

std::vector<std::uint8_t> EncodeRle(const std::vector<std::uint8_t> &input) {
    std::vector<std::uint8_t> output;
    int zero_start_index = -1;
    std::size_t previous_zero_end = 0;

    for (std::size_t i = 0; i < input.size(); ++i) {
        if (input[i] == 0) {
            if (zero_start_index < 0) {
                zero_start_index = static_cast<int>(i);
            }
        } else {
            if (zero_start_index >= 0 && static_cast<int>(i) - zero_start_index > 4) {
                WriteVariableUInt(output, static_cast<std::uint32_t>(static_cast<std::size_t>(zero_start_index) - previous_zero_end));
                output.insert(output.end(), input.begin() + static_cast<std::ptrdiff_t>(previous_zero_end), input.begin() + zero_start_index);
                WriteVariableUInt(output, static_cast<std::uint32_t>(static_cast<int>(i) - zero_start_index));
                previous_zero_end = i;
            }
            zero_start_index = -1;
        }
    }

    if (zero_start_index >= 0 && static_cast<int>(input.size()) - zero_start_index > 4) {
        WriteVariableUInt(output, static_cast<std::uint32_t>(static_cast<std::size_t>(zero_start_index) - previous_zero_end));
        output.insert(output.end(), input.begin() + static_cast<std::ptrdiff_t>(previous_zero_end), input.begin() + zero_start_index);
        WriteVariableUInt(output, static_cast<std::uint32_t>(input.size() - static_cast<std::size_t>(zero_start_index)));
    } else {
        WriteVariableUInt(output, static_cast<std::uint32_t>(input.size() - previous_zero_end));
        output.insert(output.end(), input.begin() + static_cast<std::ptrdiff_t>(previous_zero_end), input.end());
    }

    return output;
}

void ReverseAverageSampling(std::vector<std::uint8_t> &pixels, std::uint32_t width, std::uint32_t height, std::uint32_t channels) {
    if (pixels.empty()) {
        return;
    }

    const auto stride = static_cast<std::size_t>(width) * channels;

    for (std::uint32_t x = 1; x < width; ++x) {
        const auto index = static_cast<std::size_t>(x) * channels;
        for (std::uint32_t channel = 0; channel < channels; ++channel) {
            pixels[index + channel] = static_cast<std::uint8_t>(pixels[index + channel] + pixels[index - channels + channel]);
        }
    }

    for (std::uint32_t y = 1; y < height; ++y) {
        const auto row_offset = static_cast<std::size_t>(y) * stride;
        for (std::uint32_t channel = 0; channel < channels; ++channel) {
            pixels[row_offset + channel] = static_cast<std::uint8_t>(pixels[row_offset + channel] + pixels[row_offset - stride + channel]);
        }

        for (std::uint32_t x = 1; x < width; ++x) {
            const auto index = row_offset + static_cast<std::size_t>(x) * channels;
            for (std::uint32_t channel = 0; channel < channels; ++channel) {
                const auto left = pixels[index - channels + channel];
                const auto above = pixels[index - stride + channel];
                pixels[index + channel] = static_cast<std::uint8_t>(pixels[index + channel] + ((left + above) >> 1));
            }
        }
    }
}

void ApplyAverageSampling(std::vector<std::uint8_t> &pixels, std::uint32_t width, std::uint32_t height, std::uint32_t channels) {
    if (pixels.empty()) {
        return;
    }

    const auto stride = static_cast<std::size_t>(width) * channels;
    for (int y = static_cast<int>(height) - 1; y >= 0; --y) {
        const auto row_offset = static_cast<std::size_t>(y) * stride;
        for (int x = static_cast<int>(width) - 1; x >= 0; --x) {
            const auto index = row_offset + static_cast<std::size_t>(x) * channels;
            for (int channel = static_cast<int>(channels) - 1; channel >= 0; --channel) {
                int average = 0;
                if (x > 0) {
                    average += pixels[index + static_cast<std::size_t>(channel) - channels];
                }
                if (y > 0) {
                    average += pixels[index + static_cast<std::size_t>(channel) - stride];
                }
                if (x > 0 && y > 0) {
                    average /= 2;
                }
                if (average != 0) {
                    pixels[index + static_cast<std::size_t>(channel)] = static_cast<std::uint8_t>(pixels[index + static_cast<std::size_t>(channel)] - average);
                }
            }
        }
    }
}

std::array<std::uint32_t, 256> ReadFrequencies(const std::vector<std::uint8_t> &weight_data) {
    ByteReader reader(weight_data);
    std::array<std::uint32_t, 256> frequencies{};
    for (auto &frequency : frequencies) {
        frequency = ReadVariableUInt(reader);
    }
    return frequencies;
}

std::vector<std::uint8_t> SerializeFrequencies(const std::array<std::uint32_t, 256> &frequencies) {
    std::vector<std::uint8_t> output;
    for (const auto frequency : frequencies) {
        WriteVariableUInt(output, frequency);
    }
    return output;
}

std::vector<std::uint8_t> EncryptWeightData(const std::vector<std::uint8_t> &plain_data, std::uint32_t key, std::uint8_t &sum, std::uint8_t &value_xor) {
    auto encrypted = plain_data;
    sum = 0;
    value_xor = 0;
    auto mutable_key = key;
    for (std::size_t i = 0; i < plain_data.size(); ++i) {
        sum = static_cast<std::uint8_t>(sum + plain_data[i]);
        value_xor = static_cast<std::uint8_t>(value_xor ^ plain_data[i]);
        encrypted[i] = static_cast<std::uint8_t>(plain_data[i] + NextKeyByte(mutable_key));
    }
    return encrypted;
}

std::vector<std::uint8_t> PackPixels(const RasterImage &image, std::uint32_t channels) {
    std::vector<std::uint8_t> packed;
    packed.reserve(static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * channels);

    for (std::size_t i = 0; i < image.pixels.size(); i += 4) {
        packed.push_back(image.pixels[i + 0]);
        if (channels >= 2) {
            packed.push_back(image.pixels[i + 1]);
            packed.push_back(image.pixels[i + 2]);
        }
        if (channels == 4) {
            packed.push_back(image.pixels[i + 3]);
        }
    }

    return packed;
}

RasterImage UnpackPixels(const std::vector<std::uint8_t> &packed, std::uint32_t width, std::uint32_t height, std::uint32_t channels) {
    RasterImage image;
    image.width = width;
    image.height = height;
    image.has_alpha = channels == 4;
    image.pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4, 0xFF);

    for (std::size_t pixel = 0, src = 0; pixel < static_cast<std::size_t>(width) * static_cast<std::size_t>(height); ++pixel) {
        const auto dst = pixel * 4;
        if (channels == 1) {
            image.pixels[dst + 0] = packed[src];
            image.pixels[dst + 1] = packed[src];
            image.pixels[dst + 2] = packed[src];
            ++src;
        } else {
            image.pixels[dst + 0] = packed[src + 0];
            image.pixels[dst + 1] = packed[src + 1];
            image.pixels[dst + 2] = packed[src + 2];
            if (channels == 4) {
                image.pixels[dst + 3] = packed[src + 3];
            }
            src += channels;
        }
    }

    if (channels == 4) {
        image.has_alpha = false;
        for (std::size_t i = 3; i < image.pixels.size(); i += 4) {
            if (image.pixels[i] != 0xFF) {
                image.has_alpha = true;
                break;
            }
        }
    }

    return image;
}

void WriteU16(std::vector<std::uint8_t> &output, std::uint16_t value) {
    output.push_back(static_cast<std::uint8_t>(value & 0xFFu));
    output.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
}

void WriteU32(std::vector<std::uint8_t> &output, std::uint32_t value) {
    output.push_back(static_cast<std::uint8_t>(value & 0xFFu));
    output.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
    output.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFFu));
    output.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFFu));
}

}  // namespace

bool IsCbgFile(const std::filesystem::path &path) {
    if (!std::filesystem::is_regular_file(path)) {
        return false;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }

    std::array<std::uint8_t, kMagicSize> buffer{};
    file.read(reinterpret_cast<char *>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
    return file.gcount() == static_cast<std::streamsize>(buffer.size())
        && std::equal(buffer.begin(), buffer.end(), kMagic.begin());
}

RasterImage DecodeCbg(const std::filesystem::path &path) {
    ByteReader reader(ReadAllBytes(path));
    if (!StartsWithMagic(reader.data)) {
        throw std::runtime_error("Input file is not a CompressedBG image.");
    }

    reader.Skip(kMagicSize);
    const auto width = reader.ReadU16();
    const auto height = reader.ReadU16();
    const auto bits_per_pixel = reader.ReadU32();
    reader.Skip(8);

    if (bits_per_pixel != 8 && bits_per_pixel != 24 && bits_per_pixel != 32) {
        throw std::runtime_error("Only 8-bit, 24-bit and 32-bit CBG1 images are supported.");
    }

    const auto channels = bits_per_pixel / 8;
    const auto huffman_size = reader.ReadU32();
    const auto weight_data = DecryptWeightTable(reader);
    const auto compressed_stream = reader.ReadBytes(reader.Remaining());

    const auto frequencies = ReadFrequencies(weight_data);
    const auto tree = BuildHuffmanTree(frequencies);
    const auto rle_stream = HuffmanDecode(compressed_stream, huffman_size, tree);
    auto packed_pixels = DecodeRle(rle_stream, static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * channels);
    ReverseAverageSampling(packed_pixels, width, height, channels);
    return UnpackPixels(packed_pixels, width, height, channels);
}

void EncodeCbg(const RasterImage &image, const std::filesystem::path &path) {
    if (image.width == 0 || image.height == 0 || image.pixels.size() != static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 4) {
        throw std::runtime_error("RasterImage is invalid.");
    }
    if (image.width > std::numeric_limits<std::uint16_t>::max() || image.height > std::numeric_limits<std::uint16_t>::max()) {
        throw std::runtime_error("CBG1 only supports dimensions up to 65535x65535.");
    }

    bool has_real_alpha = false;
    for (std::size_t i = 3; i < image.pixels.size(); i += 4) {
        if (image.pixels[i] != 0xFF) {
            has_real_alpha = true;
            break;
        }
    }

    const std::uint32_t channels = has_real_alpha ? 4u : 3u;
    auto packed_pixels = PackPixels(image, channels);
    ApplyAverageSampling(packed_pixels, image.width, image.height, channels);
    const auto rle_data = EncodeRle(packed_pixels);

    std::array<std::uint32_t, 256> frequencies{};
    for (const auto value : rle_data) {
        ++frequencies[value];
    }

    const auto tree = BuildHuffmanTree(frequencies);
    const auto weight_plain = SerializeFrequencies(frequencies);
    std::uint8_t weight_sum = 0;
    std::uint8_t weight_xor = 0;
    constexpr std::uint32_t key = 0;
    const auto weight_encrypted = EncryptWeightData(weight_plain, key, weight_sum, weight_xor);
    const auto bitstream = HuffmanEncode(rle_data, tree);

    std::vector<std::uint8_t> output;
    output.reserve(64 + weight_encrypted.size() + bitstream.size());
    output.insert(output.end(), kMagic.begin(), kMagic.end());
    WriteU16(output, static_cast<std::uint16_t>(image.width));
    WriteU16(output, static_cast<std::uint16_t>(image.height));
    WriteU32(output, channels * 8u);
    WriteU32(output, 0);
    WriteU32(output, 0);
    WriteU32(output, static_cast<std::uint32_t>(rle_data.size()));
    WriteU32(output, key);
    WriteU32(output, static_cast<std::uint32_t>(weight_plain.size()));
    output.push_back(weight_sum);
    output.push_back(weight_xor);
    WriteU16(output, 1);
    output.insert(output.end(), weight_encrypted.begin(), weight_encrypted.end());
    output.insert(output.end(), bitstream.begin(), bitstream.end());

    WriteAllBytes(path, output);
}

}  // namespace hazuki
