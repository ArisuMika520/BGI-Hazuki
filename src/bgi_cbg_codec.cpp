#include "bgi_cbg_codec.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace hazuki
{
    namespace
    {

        constexpr std::size_t kMagicSize = 16;
        constexpr std::array<std::uint8_t, kMagicSize> kMagic = {
            0x43,
            0x6F,
            0x6D,
            0x70,
            0x72,
            0x65,
            0x73,
            0x73,
            0x65,
            0x64,
            0x42,
            0x47,
            0x5F,
            0x5F,
            0x5F,
            0x00,
        };

        struct ByteReader
        {
            explicit ByteReader(std::vector<std::uint8_t> bytes) : data(std::move(bytes)) {}

            std::uint8_t ReadU8()
            {
                EnsureAvailable(1);
                return data[position++];
            }

            std::uint16_t ReadU16()
            {
                EnsureAvailable(2);
                const auto value = static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[position]) | static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[position + 1]) << 8));
                position += 2;
                return value;
            }

            std::uint32_t ReadU32()
            {
                EnsureAvailable(4);
                const auto value = static_cast<std::uint32_t>(data[position]) | (static_cast<std::uint32_t>(data[position + 1]) << 8) | (static_cast<std::uint32_t>(data[position + 2]) << 16) | (static_cast<std::uint32_t>(data[position + 3]) << 24);
                position += 4;
                return value;
            }

            std::vector<std::uint8_t> ReadBytes(std::size_t count)
            {
                EnsureAvailable(count);
                std::vector<std::uint8_t> chunk(data.begin() + static_cast<std::ptrdiff_t>(position), data.begin() + static_cast<std::ptrdiff_t>(position + count));
                position += count;
                return chunk;
            }

            void Skip(std::size_t count)
            {
                EnsureAvailable(count);
                position += count;
            }

            std::size_t Remaining() const
            {
                return data.size() - position;
            }

            std::size_t Position() const
            {
                return position;
            }

            void Seek(std::size_t new_position)
            {
                if (new_position > data.size())
                {
                    throw std::runtime_error("Attempted to seek beyond the end of the buffer.");
                }
                position = new_position;
            }

            const std::vector<std::uint8_t> data;
            std::size_t position = 0;

        private:
            void EnsureAvailable(std::size_t count) const
            {
                if (position + count > data.size())
                {
                    throw std::runtime_error(
                        "Unexpected end of CBG data (need " + std::to_string(count) +
                        " bytes at offset " + std::to_string(position) +
                        ", but total size is " + std::to_string(data.size()) + ").");
                }
            }
        };

        struct HuffmanNode
        {
            bool is_leaf = false;
            std::uint32_t weight = 0;
            int value = -1;
            int left = -1;
            int right = -1;
            int order = -1;
        };

        struct HuffmanCode
        {
            std::uint32_t bits = 0;
            int bit_count = 0;
            std::uint32_t weight = 0;
        };

        struct HuffmanTree
        {
            std::vector<HuffmanNode> nodes;
            std::array<HuffmanCode, 256> codes{};
            int root = -1;
        };

        class BitReader
        {
        public:
            explicit BitReader(const std::vector<std::uint8_t> &data) : data_(data) {}

            bool ReadBit()
            {
                if (byte_index_ >= data_.size())
                {
                    throw std::runtime_error("Unexpected end of Huffman bitstream.");
                }

                const bool bit = (data_[byte_index_] & (0x80u >> bit_index_)) != 0;
                ++bit_index_;
                if (bit_index_ == 8)
                {
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

        class BitWriter
        {
        public:
            void Write(std::uint32_t value, int bit_count)
            {
                while (bit_count > 0)
                {
                    const int chunk = std::min(free_bits_, bit_count);
                    const std::uint32_t mask = static_cast<std::uint32_t>((1u << chunk) - 1u);
                    bit_count -= chunk;
                    free_bits_ -= chunk;
                    current_byte_ |= static_cast<std::uint8_t>(((value >> bit_count) & mask) << free_bits_);
                    if (free_bits_ == 0)
                    {
                        FlushByte();
                    }
                }
            }

            std::vector<std::uint8_t> Finish()
            {
                if (free_bits_ != 8)
                {
                    FlushByte();
                }
                return std::move(bytes_);
            }

        private:
            void FlushByte()
            {
                bytes_.push_back(current_byte_);
                current_byte_ = 0;
                free_bits_ = 8;
            }

            std::vector<std::uint8_t> bytes_;
            std::uint8_t current_byte_ = 0;
            int free_bits_ = 8;
        };

        std::vector<std::uint8_t> ReadAllBytes(const std::filesystem::path &path)
        {
            // 先用 filesystem 获取文件真实大小作为参考
            std::error_code fs_ec;
            const auto fs_size = std::filesystem::file_size(path, fs_ec);

            std::ifstream file(path, std::ios::binary);
            if (!file)
            {
                throw std::runtime_error("Failed to open input file.");
            }
            file.seekg(0, std::ios::end);
            const auto tell_pos = file.tellg();
            if (tell_pos < 0)
            {
                throw std::runtime_error("Failed to determine file size (tellg failed).");
            }
            const auto size = static_cast<std::size_t>(tell_pos);

            // 如果 tellg 结果与 filesystem::file_size 不一致，使用 filesystem 的结果
            const auto expected_size = (!fs_ec && fs_size > size) ? static_cast<std::size_t>(fs_size) : size;

            file.seekg(0, std::ios::beg);
            std::vector<std::uint8_t> data(expected_size);
            file.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(expected_size));
            const auto actually_read = static_cast<std::size_t>(file.gcount());
            if (actually_read != expected_size)
            {
                // ifstream 无法读取完整文件，回退到 Win32 API
                data.clear();
                HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (h == INVALID_HANDLE_VALUE)
                {
                    throw std::runtime_error("Failed to open file via Win32 (ifstream read " +
                                             std::to_string(actually_read) + " of " +
                                             std::to_string(expected_size) + " bytes).");
                }
                LARGE_INTEGER win_size;
                if (!GetFileSizeEx(h, &win_size))
                {
                    CloseHandle(h);
                    throw std::runtime_error("Failed to get file size via Win32.");
                }
                data.resize(static_cast<std::size_t>(win_size.QuadPart));
                DWORD bytes_read = 0;
                // 读取可能需要多次调用（大文件）
                std::size_t total_read = 0;
                while (total_read < data.size())
                {
                    const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(data.size() - total_read, 0x7FFFFFFFu));
                    if (!ReadFile(h, data.data() + total_read, chunk, &bytes_read, nullptr) || bytes_read == 0)
                    {
                        CloseHandle(h);
                        throw std::runtime_error("Win32 ReadFile failed (read " +
                                                 std::to_string(total_read) + " of " +
                                                 std::to_string(data.size()) + " bytes).");
                    }
                    total_read += bytes_read;
                }
                CloseHandle(h);
            }
            return data;
        }

        void WriteAllBytes(const std::filesystem::path &path, const std::vector<std::uint8_t> &data)
        {
            std::ofstream file(path, std::ios::binary);
            if (!file)
            {
                throw std::runtime_error("Failed to create output file.");
            }
            file.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
            if (!file)
            {
                throw std::runtime_error("Failed to write output file.");
            }
        }

        bool StartsWithMagic(const std::vector<std::uint8_t> &data)
        {
            return data.size() >= kMagicSize && std::equal(kMagic.begin(), kMagic.end(), data.begin());
        }

        std::uint8_t NextKeyByte(std::uint32_t &key)
        {
            const std::uint32_t v0 = 20021u * (key & 0xFFFFu);
            std::uint32_t v1 = key >> 16;
            v1 = v1 * 20021u + key * 346u;
            v1 = (v1 + (v0 >> 16)) & 0xFFFFu;
            key = (v1 << 16) + (v0 & 0xFFFFu) + 1u;
            return static_cast<std::uint8_t>(v1 & 0xFFu);
        }

        std::uint32_t ReadVariableUInt(ByteReader &reader)
        {
            std::uint32_t result = 0;
            std::uint32_t shift = 0;
            while (true)
            {
                const auto current = reader.ReadU8();
                result |= static_cast<std::uint32_t>(current & 0x7Fu) << shift;
                if ((current & 0x80u) == 0)
                {
                    return result;
                }
                shift += 7;
                if (shift > 28)
                {
                    throw std::runtime_error("Variable-length integer is too large.");
                }
            }
        }

        void WriteVariableUInt(std::vector<std::uint8_t> &output, std::uint32_t value)
        {
            do
            {
                std::uint8_t byte = static_cast<std::uint8_t>(value & 0x7Fu);
                value >>= 7;
                if (value != 0)
                {
                    byte |= 0x80u;
                }
                output.push_back(byte);
            } while (value != 0);
        }

        std::vector<std::uint8_t> DecryptWeightTable(ByteReader &reader)
        {
            std::uint32_t key = reader.ReadU32();
            const auto data_size = reader.ReadU32();
            if (data_size > reader.Remaining() + 4)
            {
                throw std::runtime_error(
                    "CBG weight table size (" + std::to_string(data_size) +
                    ") exceeds remaining file data (" + std::to_string(reader.Remaining()) +
                    " bytes). The file may be truncated or use an unsupported format variant.");
            }
            const auto expected_sum = reader.ReadU8();
            const auto expected_xor = reader.ReadU8();
            reader.Skip(2);
            auto encrypted = reader.ReadBytes(data_size);

            std::uint8_t actual_sum = 0;
            std::uint8_t actual_xor = 0;
            for (auto &byte : encrypted)
            {
                byte = static_cast<std::uint8_t>(byte - NextKeyByte(key));
                actual_sum = static_cast<std::uint8_t>(actual_sum + byte);
                actual_xor = static_cast<std::uint8_t>(actual_xor ^ byte);
            }

            if (actual_sum != expected_sum || actual_xor != expected_xor)
            {
                throw std::runtime_error("CBG weight-table checksum mismatch.");
            }

            return encrypted;
        }

        HuffmanTree BuildHuffmanTree(const std::array<std::uint32_t, 256> &frequencies)
        {
            HuffmanTree tree;
            std::vector<int> active;
            tree.nodes.reserve(512);

            for (int symbol = 0; symbol < 256; ++symbol)
            {
                if (frequencies[symbol] == 0)
                {
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

            if (active.empty())
            {
                throw std::runtime_error("Cannot build a Huffman tree from empty data.");
            }

            int next_order = 256;
            auto compare = [&tree](int left, int right)
            {
                const auto &a = tree.nodes[static_cast<std::size_t>(left)];
                const auto &b = tree.nodes[static_cast<std::size_t>(right)];
                if (a.weight != b.weight)
                {
                    return a.weight < b.weight;
                }
                return a.order < b.order;
            };

            while (active.size() > 1)
            {
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

            const auto assign_codes = [&](const auto &self, int node_index, std::uint32_t bits, int bit_count) -> void
            {
                auto &node = tree.nodes[static_cast<std::size_t>(node_index)];
                if (node.is_leaf)
                {
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

        std::vector<std::uint8_t> HuffmanDecode(const std::vector<std::uint8_t> &bitstream, std::size_t output_size, const HuffmanTree &tree)
        {
            std::vector<std::uint8_t> output;
            output.reserve(output_size);

            const auto &root = tree.nodes[static_cast<std::size_t>(tree.root)];
            if (root.is_leaf)
            {
                output.assign(output_size, static_cast<std::uint8_t>(root.value));
                return output;
            }

            BitReader reader(bitstream);
            while (output.size() < output_size)
            {
                int node_index = tree.root;
                while (!tree.nodes[static_cast<std::size_t>(node_index)].is_leaf)
                {
                    const bool bit = reader.ReadBit();
                    node_index = bit ? tree.nodes[static_cast<std::size_t>(node_index)].right : tree.nodes[static_cast<std::size_t>(node_index)].left;
                    if (node_index < 0)
                    {
                        throw std::runtime_error("Encountered an invalid Huffman branch.");
                    }
                }
                output.push_back(static_cast<std::uint8_t>(tree.nodes[static_cast<std::size_t>(node_index)].value));
            }

            return output;
        }

        std::vector<std::uint8_t> HuffmanEncode(const std::vector<std::uint8_t> &input, const HuffmanTree &tree)
        {
            BitWriter writer;
            for (const auto value : input)
            {
                const auto &code = tree.codes[static_cast<std::size_t>(value)];
                writer.Write(code.bits, code.bit_count);
            }
            return writer.Finish();
        }

        std::vector<std::uint8_t> DecodeRle(const std::vector<std::uint8_t> &input, std::size_t output_size)
        {
            ByteReader reader(input);
            std::vector<std::uint8_t> output;
            output.reserve(output_size);
            bool zero_flag = false;

            while (reader.Remaining() > 0 && output.size() < output_size)
            {
                auto count = static_cast<std::size_t>(ReadVariableUInt(reader));
                const auto remaining = output_size - output.size();
                count = std::min(count, remaining);

                if (zero_flag)
                {
                    output.insert(output.end(), count, 0);
                }
                else
                {
                    auto bytes = reader.ReadBytes(count);
                    output.insert(output.end(), bytes.begin(), bytes.end());
                }
                zero_flag = !zero_flag;
            }

            if (output.size() != output_size)
            {
                throw std::runtime_error("Decoded RLE payload has an unexpected size.");
            }

            return output;
        }

        std::vector<std::uint8_t> EncodeRle(const std::vector<std::uint8_t> &input)
        {
            std::vector<std::uint8_t> output;
            int zero_start_index = -1;
            std::size_t previous_zero_end = 0;

            for (std::size_t i = 0; i < input.size(); ++i)
            {
                if (input[i] == 0)
                {
                    if (zero_start_index < 0)
                    {
                        zero_start_index = static_cast<int>(i);
                    }
                }
                else
                {
                    if (zero_start_index >= 0 && static_cast<int>(i) - zero_start_index > 4)
                    {
                        WriteVariableUInt(output, static_cast<std::uint32_t>(static_cast<std::size_t>(zero_start_index) - previous_zero_end));
                        output.insert(output.end(), input.begin() + static_cast<std::ptrdiff_t>(previous_zero_end), input.begin() + zero_start_index);
                        WriteVariableUInt(output, static_cast<std::uint32_t>(static_cast<int>(i) - zero_start_index));
                        previous_zero_end = i;
                    }
                    zero_start_index = -1;
                }
            }

            if (zero_start_index >= 0 && static_cast<int>(input.size()) - zero_start_index > 4)
            {
                WriteVariableUInt(output, static_cast<std::uint32_t>(static_cast<std::size_t>(zero_start_index) - previous_zero_end));
                output.insert(output.end(), input.begin() + static_cast<std::ptrdiff_t>(previous_zero_end), input.begin() + zero_start_index);
                WriteVariableUInt(output, static_cast<std::uint32_t>(input.size() - static_cast<std::size_t>(zero_start_index)));
            }
            else
            {
                WriteVariableUInt(output, static_cast<std::uint32_t>(input.size() - previous_zero_end));
                output.insert(output.end(), input.begin() + static_cast<std::ptrdiff_t>(previous_zero_end), input.end());
            }

            return output;
        }

        void ReverseAverageSampling(std::vector<std::uint8_t> &pixels, std::uint32_t width, std::uint32_t height, std::uint32_t channels)
        {
            if (pixels.empty())
            {
                return;
            }

            const auto stride = static_cast<std::size_t>(width) * channels;

            for (std::uint32_t x = 1; x < width; ++x)
            {
                const auto index = static_cast<std::size_t>(x) * channels;
                for (std::uint32_t channel = 0; channel < channels; ++channel)
                {
                    pixels[index + channel] = static_cast<std::uint8_t>(pixels[index + channel] + pixels[index - channels + channel]);
                }
            }

            for (std::uint32_t y = 1; y < height; ++y)
            {
                const auto row_offset = static_cast<std::size_t>(y) * stride;
                for (std::uint32_t channel = 0; channel < channels; ++channel)
                {
                    pixels[row_offset + channel] = static_cast<std::uint8_t>(pixels[row_offset + channel] + pixels[row_offset - stride + channel]);
                }

                for (std::uint32_t x = 1; x < width; ++x)
                {
                    const auto index = row_offset + static_cast<std::size_t>(x) * channels;
                    for (std::uint32_t channel = 0; channel < channels; ++channel)
                    {
                        const auto left = pixels[index - channels + channel];
                        const auto above = pixels[index - stride + channel];
                        pixels[index + channel] = static_cast<std::uint8_t>(pixels[index + channel] + ((left + above) >> 1));
                    }
                }
            }
        }

        void ApplyAverageSampling(std::vector<std::uint8_t> &pixels, std::uint32_t width, std::uint32_t height, std::uint32_t channels)
        {
            if (pixels.empty())
            {
                return;
            }

            const auto stride = static_cast<std::size_t>(width) * channels;
            for (int y = static_cast<int>(height) - 1; y >= 0; --y)
            {
                const auto row_offset = static_cast<std::size_t>(y) * stride;
                for (int x = static_cast<int>(width) - 1; x >= 0; --x)
                {
                    const auto index = row_offset + static_cast<std::size_t>(x) * channels;
                    for (int channel = static_cast<int>(channels) - 1; channel >= 0; --channel)
                    {
                        int average = 0;
                        if (x > 0)
                        {
                            average += pixels[index + static_cast<std::size_t>(channel) - channels];
                        }
                        if (y > 0)
                        {
                            average += pixels[index + static_cast<std::size_t>(channel) - stride];
                        }
                        if (x > 0 && y > 0)
                        {
                            average /= 2;
                        }
                        if (average != 0)
                        {
                            pixels[index + static_cast<std::size_t>(channel)] = static_cast<std::uint8_t>(pixels[index + static_cast<std::size_t>(channel)] - average);
                        }
                    }
                }
            }
        }

        std::array<std::uint32_t, 256> ReadFrequencies(const std::vector<std::uint8_t> &weight_data)
        {
            ByteReader reader(weight_data);
            std::array<std::uint32_t, 256> frequencies{};
            for (auto &frequency : frequencies)
            {
                frequency = ReadVariableUInt(reader);
            }
            return frequencies;
        }

        std::vector<std::uint8_t> SerializeFrequencies(const std::array<std::uint32_t, 256> &frequencies)
        {
            std::vector<std::uint8_t> output;
            for (const auto frequency : frequencies)
            {
                WriteVariableUInt(output, frequency);
            }
            return output;
        }

        std::vector<std::uint8_t> EncryptWeightData(const std::vector<std::uint8_t> &plain_data, std::uint32_t key, std::uint8_t &sum, std::uint8_t &value_xor)
        {
            auto encrypted = plain_data;
            sum = 0;
            value_xor = 0;
            auto mutable_key = key;
            for (std::size_t i = 0; i < plain_data.size(); ++i)
            {
                sum = static_cast<std::uint8_t>(sum + plain_data[i]);
                value_xor = static_cast<std::uint8_t>(value_xor ^ plain_data[i]);
                encrypted[i] = static_cast<std::uint8_t>(plain_data[i] + NextKeyByte(mutable_key));
            }
            return encrypted;
        }

        std::vector<std::uint8_t> PackPixels(const RasterImage &image, std::uint32_t channels)
        {
            std::vector<std::uint8_t> packed;
            packed.reserve(static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * channels);

            for (std::size_t i = 0; i < image.pixels.size(); i += 4)
            {
                packed.push_back(image.pixels[i + 0]);
                if (channels >= 2)
                {
                    packed.push_back(image.pixels[i + 1]);
                    packed.push_back(image.pixels[i + 2]);
                }
                if (channels == 4)
                {
                    packed.push_back(image.pixels[i + 3]);
                }
            }

            return packed;
        }

        RasterImage UnpackPixels(const std::vector<std::uint8_t> &packed, std::uint32_t width, std::uint32_t height, std::uint32_t channels)
        {
            RasterImage image;
            image.width = width;
            image.height = height;
            image.has_alpha = channels == 4;
            image.pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4, 0xFF);

            for (std::size_t pixel = 0, src = 0; pixel < static_cast<std::size_t>(width) * static_cast<std::size_t>(height); ++pixel)
            {
                const auto dst = pixel * 4;
                if (channels == 1)
                {
                    image.pixels[dst + 0] = packed[src];
                    image.pixels[dst + 1] = packed[src];
                    image.pixels[dst + 2] = packed[src];
                    ++src;
                }
                else
                {
                    image.pixels[dst + 0] = packed[src + 0];
                    image.pixels[dst + 1] = packed[src + 1];
                    image.pixels[dst + 2] = packed[src + 2];
                    if (channels == 4)
                    {
                        image.pixels[dst + 3] = packed[src + 3];
                    }
                    src += channels;
                }
            }

            if (channels == 4)
            {
                image.has_alpha = false;
                for (std::size_t i = 3; i < image.pixels.size(); i += 4)
                {
                    if (image.pixels[i] != 0xFF)
                    {
                        image.has_alpha = true;
                        break;
                    }
                }
            }

            return image;
        }

        void WriteU16(std::vector<std::uint8_t> &output, std::uint16_t value)
        {
            output.push_back(static_cast<std::uint8_t>(value & 0xFFu));
            output.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
        }

        void WriteU32(std::vector<std::uint8_t> &output, std::uint32_t value)
        {
            output.push_back(static_cast<std::uint8_t>(value & 0xFFu));
            output.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
            output.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFFu));
            output.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFFu));
        }

        // ===================== CBG Version 2 (DCT-based) =====================

        // MSB-first bit reader for v2 Huffman decoding
        class MsbBitReader
        {
        public:
            MsbBitReader(const std::uint8_t *data, std::size_t size)
                : data_(data), size_(size) {}

            int GetNextBit()
            {
                if (cache_bits_ == 0)
                {
                    if (byte_pos_ >= size_)
                        return -1;
                    cache_ = data_[byte_pos_++];
                    cache_bits_ = 8;
                }
                --cache_bits_;
                return (cache_ >> cache_bits_) & 1;
            }

            int GetBits(int count)
            {
                int result = 0;
                for (int i = 0; i < count; ++i)
                {
                    int bit = GetNextBit();
                    if (bit < 0)
                        return -1;
                    result = (result << 1) | bit;
                }
                return result;
            }

            int CacheBits() const { return cache_bits_; }

            void AlignByte()
            {
                if (cache_bits_ > 0 && cache_bits_ < 8)
                {
                    cache_bits_ = 0;
                }
            }

        private:
            const std::uint8_t *data_;
            std::size_t size_;
            std::size_t byte_pos_ = 0;
            int cache_ = 0;
            int cache_bits_ = 0;
        };

        // Huffman tree for v2 — built from weight array with v2 tie-breaking
        struct HuffmanTreeV2
        {
            struct Node
            {
                bool valid = false;
                bool is_parent = false;
                std::uint32_t weight = 0;
                int left = -1;
                int right = -1;
            };

            std::vector<Node> nodes;

            explicit HuffmanTreeV2(const std::vector<std::uint32_t> &weights)
            {
                nodes.reserve(weights.size() * 2);
                std::uint32_t root_weight = 0;
                for (std::size_t i = 0; i < weights.size(); ++i)
                {
                    Node n;
                    n.valid = weights[i] != 0;
                    n.weight = weights[i];
                    n.is_parent = false;
                    nodes.push_back(n);
                    root_weight += weights[i];
                }
                if (root_weight == 0)
                {
                    throw std::runtime_error("CBG v2: all Huffman weights are zero.");
                }

                int child_idx[2];
                for (;;)
                {
                    std::uint32_t combined_weight = 0;
                    for (int c = 0; c < 2; ++c)
                    {
                        std::uint32_t min_w = UINT32_MAX;
                        child_idx[c] = -1;
                        // v2 tie-breaking: pick the first valid node found, then scan rest
                        int n = 0;
                        for (; n < static_cast<int>(nodes.size()); ++n)
                        {
                            if (nodes[n].valid)
                            {
                                min_w = nodes[n].weight;
                                child_idx[c] = n;
                                ++n;
                                break;
                            }
                        }
                        n = std::max(n, c + 1);
                        for (; n < static_cast<int>(nodes.size()); ++n)
                        {
                            if (nodes[n].valid && nodes[n].weight < min_w)
                            {
                                min_w = nodes[n].weight;
                                child_idx[c] = n;
                            }
                        }
                        if (child_idx[c] >= 0)
                        {
                            nodes[child_idx[c]].valid = false;
                            combined_weight += nodes[child_idx[c]].weight;
                        }
                    }
                    Node parent;
                    parent.valid = true;
                    parent.is_parent = true;
                    parent.weight = combined_weight;
                    parent.left = child_idx[0];
                    parent.right = child_idx[1];
                    nodes.push_back(parent);
                    if (combined_weight >= root_weight)
                        break;
                }
            }

            int DecodeToken(MsbBitReader &reader) const
            {
                int idx = static_cast<int>(nodes.size()) - 1;
                while (nodes[idx].is_parent)
                {
                    int bit = reader.GetNextBit();
                    if (bit < 0)
                        throw std::runtime_error("CBG v2: unexpected end of Huffman bitstream.");
                    idx = (bit == 0) ? nodes[idx].left : nodes[idx].right;
                    if (idx < 0)
                        throw std::runtime_error("CBG v2: invalid Huffman tree node.");
                }
                return idx;
            }
        };

        // Read varint-encoded weight table from a byte stream at a given position
        std::vector<std::uint32_t> ReadWeightTableV2(const std::uint8_t *data, std::size_t data_size,
                                                     std::size_t &pos, int count)
        {
            std::vector<std::uint32_t> weights(count);
            for (int i = 0; i < count; ++i)
            {
                std::uint32_t val = 0;
                std::uint32_t shift = 0;
                while (true)
                {
                    if (pos >= data_size)
                        throw std::runtime_error("CBG v2: unexpected end while reading weight table.");
                    std::uint8_t b = data[pos++];
                    val |= static_cast<std::uint32_t>(b & 0x7F) << shift;
                    if ((b & 0x80) == 0)
                        break;
                    shift += 7;
                    if (shift > 28)
                        throw std::runtime_error("CBG v2: varint overflow in weight table.");
                }
                weights[i] = val;
            }
            return weights;
        }

        static const float kDctBasisTable[64] = {
            1.00000000f, 1.38703990f, 1.30656302f, 1.17587554f, 1.00000000f, 0.78569496f, 0.54119611f, 0.27589938f,
            1.38703990f, 1.92387950f, 1.81225491f, 1.63098633f, 1.38703990f, 1.08979023f, 0.75066054f, 0.38268343f,
            1.30656302f, 1.81225491f, 1.70710683f, 1.53635550f, 1.30656302f, 1.02655995f, 0.70710677f, 0.36047992f,
            1.17587554f, 1.63098633f, 1.53635550f, 1.38268340f, 1.17587554f, 0.92387950f, 0.63637930f, 0.32442334f,
            1.00000000f, 1.38703990f, 1.30656302f, 1.17587554f, 1.00000000f, 0.78569496f, 0.54119611f, 0.27589938f,
            0.78569496f, 1.08979023f, 1.02655995f, 0.92387950f, 0.78569496f, 0.61731654f, 0.42521504f, 0.21677275f,
            0.54119611f, 0.75066054f, 0.70710677f, 0.63637930f, 0.54119611f, 0.42521504f, 0.29289323f, 0.14931567f,
            0.27589938f, 0.38268343f, 0.36047992f, 0.32442334f, 0.27589938f, 0.21677275f, 0.14931567f, 0.07612047f,
        };

        static const std::uint8_t kBlockFillOrder[64] = {
            0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
            12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
            35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
            58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
        };

        static inline short FloatToShort(float f)
        {
            int a = 0x80 + (static_cast<int>(f) >> 3);
            if (a <= 0) return 0;
            if (a <= 0xFF) return static_cast<short>(a);
            if (a < 0x180) return 0xFF;
            return 0;
        }

        static inline std::uint8_t FloatToByte(float f)
        {
            if (f >= 255.0f) return 0xFF;
            if (f <= 0.0f) return 0;
            return static_cast<std::uint8_t>(f);
        }

        // IDCT for one 8x8 block (AAN algorithm — same as GARBro)
        static void DecodeDCT(int channel, const short *data, int src,
                              float dct[2][64], short ycbcr[][3], float tmp[8][8])
        {
            int d = channel > 0 ? 1 : 0;
            for (int i = 0; i < 8; ++i)
            {
                if (data[src + 8 + i] == 0 && data[src + 16 + i] == 0 && data[src + 24 + i] == 0 &&
                    data[src + 32 + i] == 0 && data[src + 40 + i] == 0 && data[src + 48 + i] == 0 &&
                    data[src + 56 + i] == 0)
                {
                    float t = data[src + i] * dct[d][i];
                    for (int j = 0; j < 8; ++j)
                        tmp[j][i] = t;
                    continue;
                }

                float v1 = data[src + i] * dct[d][i];
                float v2 = data[src + 8 + i] * dct[d][8 + i];
                float v3 = data[src + 16 + i] * dct[d][16 + i];
                float v4 = data[src + 24 + i] * dct[d][24 + i];
                float v5 = data[src + 32 + i] * dct[d][32 + i];
                float v6 = data[src + 40 + i] * dct[d][40 + i];
                float v7 = data[src + 48 + i] * dct[d][48 + i];
                float v8 = data[src + 56 + i] * dct[d][56 + i];

                float v10 = v1 + v5, v11 = v1 - v5;
                float v12 = v3 + v7, v13 = (v3 - v7) * 1.414213562f - v12;
                v1 = v10 + v12; float v7_ = v10 - v12;
                float v3_ = v11 + v13, v5_ = v11 - v13;

                float v14 = v2 + v8, v15 = v2 - v8;
                float v16 = v6 + v4, v17 = v6 - v4;
                v8 = v14 + v16;
                v11 = (v14 - v16) * 1.414213562f;
                float v9 = (v17 + v15) * 1.847759065f;
                v10 = 1.082392200f * v15 - v9;
                v13 = -2.613125930f * v17 + v9;
                v6 = v13 - v8; float v4_ = v11 - v6; v2 = v10 + v4_;

                tmp[0][i] = v1 + v8;
                tmp[1][i] = v3_ + v6;
                tmp[2][i] = v5_ + v4_;
                tmp[3][i] = v7_ - v2;
                tmp[4][i] = v7_ + v2;
                tmp[5][i] = v5_ - v4_;
                tmp[6][i] = v3_ - v6;
                tmp[7][i] = v1 - v8;
            }

            int dst = 0;
            for (int i = 0; i < 8; ++i)
            {
                float v10 = tmp[i][0] + tmp[i][4], v11 = tmp[i][0] - tmp[i][4];
                float v12 = tmp[i][2] + tmp[i][6];
                float v13 = (tmp[i][2] - tmp[i][6]) * 1.414213562f - v12;
                float v14 = tmp[i][1] + tmp[i][7], v15 = tmp[i][1] - tmp[i][7];
                float v16 = tmp[i][5] + tmp[i][3], v17 = tmp[i][5] - tmp[i][3];

                float v1 = v10 + v12, v7 = v10 - v12;
                float v3 = v11 + v13, v5 = v11 - v13;
                float v8 = v14 + v16;
                v11 = (v14 - v16) * 1.414213562f;
                float v9 = (v17 + v15) * 1.847759065f;
                v10 = v9 - v15 * 1.082392200f;
                v13 = v9 - v17 * 2.613125930f;
                float v6 = v13 - v8, v4 = v11 - v6, v2 = v10 - v4;

                ycbcr[dst++][channel] = FloatToShort(v1 + v8);
                ycbcr[dst++][channel] = FloatToShort(v3 + v6);
                ycbcr[dst++][channel] = FloatToShort(v5 + v4);
                ycbcr[dst++][channel] = FloatToShort(v7 + v2);
                ycbcr[dst++][channel] = FloatToShort(v7 - v2);
                ycbcr[dst++][channel] = FloatToShort(v5 - v4);
                ycbcr[dst++][channel] = FloatToShort(v3 - v6);
                ycbcr[dst++][channel] = FloatToShort(v1 - v8);
            }
        }

        // Decode one strip (8 rows) of RGB blocks
        static void DecodeRGBBlocks(const std::uint8_t *block_data, std::size_t block_size,
                                    const HuffmanTreeV2 &tree1, const HuffmanTreeV2 &tree2,
                                    float dct[2][64], int width, std::uint8_t *output, int dst_offset)
        {
            MsbBitReader reader(block_data, block_size);
            // Read block_size (varint from byte stream)
            std::size_t varint_pos = 0;
            std::uint32_t block_count_val = 0;
            {
                std::uint32_t shift = 0;
                while (varint_pos < block_size)
                {
                    std::uint8_t b = block_data[varint_pos++];
                    block_count_val |= static_cast<std::uint32_t>(b & 0x7F) << shift;
                    if ((b & 0x80) == 0) break;
                    shift += 7;
                }
            }
            if (block_count_val == 0) return;

            auto color_data = std::vector<short>(block_count_val, 0);

            // Re-create the MsbBitReader starting after the varint
            MsbBitReader bits(block_data + varint_pos, block_size - varint_pos);

            // Decode DC coefficients (every 64th element)
            int acc = 0;
            for (std::size_t i = 0; i < block_count_val; i += 64)
            {
                int count = tree1.DecodeToken(bits);
                if (count != 0)
                {
                    int v = bits.GetBits(count);
                    if (v >= 0 && count > 0 && (v >> (count - 1)) == 0)
                        v = (-1 << count | v) + 1;
                    acc += v;
                }
                color_data[i] = static_cast<short>(acc);
            }

            // Align to byte boundary
            if (bits.CacheBits() & 7)
                bits.GetBits(bits.CacheBits() & 7);

            // Decode AC coefficients
            for (std::size_t i = 0; i < block_count_val; i += 64)
            {
                int index = 1;
                while (index < 64)
                {
                    int code = tree2.DecodeToken(bits);
                    if (code == 0) break;
                    if (code == 0xF)
                    {
                        index += 0x10;
                        continue;
                    }
                    index += code & 0xF;
                    if (index >= 64) break;
                    int nbits = code >> 4;
                    int v = bits.GetBits(nbits);
                    if (nbits != 0 && v >= 0 && (v >> (nbits - 1)) == 0)
                        v = (-1 << nbits | v) + 1;
                    color_data[i + kBlockFillOrder[index]] = static_cast<short>(v);
                    ++index;
                }
            }

            // IDCT + YCbCr→RGB
            short ycbcr[64][3];
            float tmp[8][8];
            int block_count_x = width / 8;
            int blk_dst = dst_offset;
            for (int bx = 0; bx < block_count_x; ++bx)
            {
                int src = bx * 64;
                std::memset(ycbcr, 0, sizeof(ycbcr));
                for (int ch = 0; ch < 3; ++ch)
                {
                    DecodeDCT(ch, color_data.data(), src, dct, ycbcr, tmp);
                    src += width * 8; // each channel's blocks are laid out sequentially per strip
                }
                for (int j = 0; j < 64; ++j)
                {
                    float cy = ycbcr[j][0];
                    float cb = ycbcr[j][1];
                    float cr = ycbcr[j][2];
                    float r = cy + 1.402f * cr - 178.956f;
                    float g = cy - 0.34414f * cb - 0.71414f * cr + 135.95984f;
                    float b = cy + 1.772f * cb - 226.316f;
                    int y = j >> 3;
                    int x = j & 7;
                    int p = blk_dst + (y * width + x) * 4;
                    output[p + 0] = FloatToByte(b);
                    output[p + 1] = FloatToByte(g);
                    output[p + 2] = FloatToByte(r);
                    output[p + 3] = 0xFF;
                }
                blk_dst += 32; // advance by 8 pixels * 4 bytes
            }
        }

        // Decode alpha channel for v2
        static void DecodeAlphaV2(const std::uint8_t *data, std::size_t size,
                                  int width, std::uint8_t *output, std::size_t output_size)
        {
            if (size < 4) return;
            std::uint32_t marker = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
            if (marker != 1) return;
            std::size_t src = 4;
            std::size_t dst = 3; // alpha channel is at byte offset 3 in BGRA
            int ctl = 1 << 1;
            while (dst < output_size && src < size)
            {
                ctl >>= 1;
                if (ctl == 1)
                {
                    if (src >= size) break;
                    ctl = data[src++] | 0x100;
                }
                if (ctl & 1)
                {
                    if (src + 1 >= size) break;
                    int v = data[src] | (data[src + 1] << 8);
                    src += 2;
                    int xoff = v & 0x3F;
                    if (xoff > 0x1F) xoff |= ~0x3F; // sign extend
                    int yoff = (v >> 6) & 7;
                    if (yoff != 0) yoff |= ~7; // sign extend
                    int count = ((v >> 9) & 0x7F) + 3;
                    std::size_t ref = static_cast<std::size_t>(static_cast<int>(dst) + (xoff + yoff * width) * 4);
                    if (ref >= dst) break;
                    for (int i = 0; i < count && dst < output_size; ++i)
                    {
                        if (ref < output_size)
                            output[dst] = output[ref];
                        ref += 4;
                        dst += 4;
                    }
                }
                else
                {
                    if (src >= size) break;
                    output[dst] = data[src++];
                    dst += 4;
                }
            }
        }

        // ===================== End of CBG v2 helpers =====================

    } // namespace

    bool IsCbgFile(const std::filesystem::path &path)
    {
        if (!std::filesystem::is_regular_file(path))
        {
            return false;
        }

        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            return false;
        }

        std::array<std::uint8_t, kMagicSize> buffer{};
        file.read(reinterpret_cast<char *>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        return file.gcount() == static_cast<std::streamsize>(buffer.size()) && std::equal(buffer.begin(), buffer.end(), kMagic.begin());
    }

    RasterImage DecodeCbg(const std::filesystem::path &path)
    {
        ByteReader reader(ReadAllBytes(path));
        if (!StartsWithMagic(reader.data))
        {
            throw std::runtime_error("Input file is not a CompressedBG image.");
        }

        if (reader.data.size() < 48)
        {
            throw std::runtime_error("CBG file is too small (" + std::to_string(reader.data.size()) + " bytes, minimum 48).");
        }

        reader.Skip(kMagicSize);
        const auto width = reader.ReadU16();
        const auto height = reader.ReadU16();
        const auto bits_per_pixel = reader.ReadU32();
        reader.Skip(8);

        const auto channels = bits_per_pixel / 8;

        // Read common header fields
        const auto intermediate_length = reader.ReadU32(); // huffman_size for v1, 0 for v2
        const auto key_val = reader.ReadU32();
        const auto enc_length = reader.ReadU32();
        const auto checksum_sum = reader.ReadU8();
        const auto checksum_xor = reader.ReadU8();
        const auto version = reader.ReadU16();

        // Decrypt the encoded data
        if (enc_length > reader.Remaining())
        {
            throw std::runtime_error(
                "CBG encrypted data size (" + std::to_string(enc_length) +
                ") exceeds remaining file data (" + std::to_string(reader.Remaining()) + " bytes).");
        }
        auto encrypted = reader.ReadBytes(enc_length);
        {
            auto dec_key = key_val;
            std::uint8_t actual_sum = 0;
            std::uint8_t actual_xor = 0;
            for (auto &b : encrypted)
            {
                b = static_cast<std::uint8_t>(b - NextKeyByte(dec_key));
                actual_sum = static_cast<std::uint8_t>(actual_sum + b);
                actual_xor = static_cast<std::uint8_t>(actual_xor ^ b);
            }
            if (actual_sum != checksum_sum || actual_xor != checksum_xor)
            {
                throw std::runtime_error("CBG weight-table checksum mismatch.");
            }
        }

        if (version < 2)
        {
            // ============ CBG v1: Huffman + RLE ============
            if (bits_per_pixel != 8 && bits_per_pixel != 16 && bits_per_pixel != 24 && bits_per_pixel != 32)
            {
                throw std::runtime_error("Only 8/16/24/32-bit CBG v1 images are supported.");
            }

            const auto compressed_stream = reader.ReadBytes(reader.Remaining());
            ByteReader weight_reader(std::move(encrypted));
            std::array<std::uint32_t, 256> frequencies{};
            for (auto &f : frequencies)
                f = ReadVariableUInt(weight_reader);

            const auto tree = BuildHuffmanTree(frequencies);
            const auto rle_stream = HuffmanDecode(compressed_stream, intermediate_length, tree);
            auto packed_pixels = DecodeRle(rle_stream, static_cast<std::size_t>(width) * height * channels);
            ReverseAverageSampling(packed_pixels, width, height, channels);
            return UnpackPixels(packed_pixels, width, height, channels);
        }
        else if (version == 2)
        {
            // ============ CBG v2: DCT-based (JPEG-like) ============
            if (bits_per_pixel != 8 && bits_per_pixel != 24 && bits_per_pixel != 32)
            {
                throw std::runtime_error("Only 8/24/32-bit CBG v2 images are supported.");
            }
            if (enc_length < 0x80)
            {
                throw std::runtime_error("CBG v2: encrypted data too short for DCT coefficients.");
            }

            // Build DCT coefficient tables from decrypted data (first 128 bytes → 2x64 floats)
            float dct[2][64];
            for (int i = 0; i < 0x80; ++i)
            {
                dct[i >> 6][i & 0x3F] = static_cast<float>(encrypted[i]) * kDctBasisTable[i & 0x3F];
            }

            // Padded dimensions (must be multiples of 8)
            const int padded_w = (width + 7) & ~7;
            const int padded_h = (height + 7) & ~7;

            // Read two Huffman weight tables from the UNENCRYPTED stream (after encrypted data)
            const std::uint8_t *stream_data = reader.data.data();
            std::size_t stream_size = reader.data.size();
            std::size_t pos = reader.Position();
            const std::size_t base_offset = pos; // position right after encrypted data

            auto weights1 = ReadWeightTableV2(stream_data, stream_size, pos, 0x10);
            auto weights2 = ReadWeightTableV2(stream_data, stream_size, pos, 0xB0);

            HuffmanTreeV2 tree1(weights1);
            HuffmanTreeV2 tree2(weights2);

            // Read block offsets
            const int y_blocks = padded_h / 8;
            std::vector<std::int32_t> offsets(y_blocks + 1);
            const int input_base = static_cast<int>((pos + (y_blocks + 1) * 4) - base_offset);

            for (int i = 0; i <= y_blocks; ++i)
            {
                if (pos + 4 > stream_size)
                    throw std::runtime_error("CBG v2: unexpected end reading block offsets.");
                offsets[i] = static_cast<std::int32_t>(
                    stream_data[pos] | (stream_data[pos + 1] << 8) |
                    (stream_data[pos + 2] << 16) | (stream_data[pos + 3] << 24));
                offsets[i] -= input_base;
                pos += 4;
            }

            // Remaining data is the compressed block data
            const std::uint8_t *block_base = stream_data + pos;
            const std::size_t block_total_size = stream_size - pos;

            const int pad_skip = ((padded_w >> 3) + 7) >> 3;

            // Allocate output in BGRA format (padded dimensions)
            std::vector<std::uint8_t> output(static_cast<std::size_t>(padded_w) * padded_h * 4, 0xFF);

            // Decode each 8-row strip
            for (int i = 0; i < y_blocks; ++i)
            {
                int block_offset = offsets[i] + pad_skip;
                int next_offset = (i + 1 == y_blocks) ? static_cast<int>(block_total_size) : offsets[i + 1];
                if (block_offset < 0 || block_offset >= static_cast<int>(block_total_size))
                    continue;
                int blk_len = next_offset - block_offset;
                if (blk_len <= 0) continue;

                int dst = i * padded_w * 8 * 4; // 8 rows * width * 4 bytes
                DecodeRGBBlocks(block_base + block_offset,
                                static_cast<std::size_t>(blk_len),
                                tree1, tree2, dct, padded_w,
                                output.data(), dst);
            }

            // Decode alpha if 32-bit
            bool has_alpha = false;
            if (bits_per_pixel == 32)
            {
                int alpha_offset = offsets[y_blocks];
                if (alpha_offset >= 0 && alpha_offset < static_cast<int>(block_total_size))
                {
                    DecodeAlphaV2(block_base + alpha_offset,
                                  block_total_size - alpha_offset,
                                  padded_w, output.data(), output.size());
                    // Check if any alpha != 0xFF
                    for (std::size_t pi = 3; pi < output.size(); pi += 4)
                    {
                        if (output[pi] != 0xFF)
                        {
                            has_alpha = true;
                            break;
                        }
                    }
                }
            }

            // If padded dimensions differ from original, crop
            RasterImage image;
            image.width = width;
            image.height = height;
            image.has_alpha = has_alpha;
            image.pixels.resize(static_cast<std::size_t>(width) * height * 4);

            if (padded_w == width && padded_h == height)
            {
                image.pixels = std::move(output);
            }
            else
            {
                for (int y = 0; y < height; ++y)
                {
                    std::memcpy(image.pixels.data() + y * width * 4,
                                output.data() + y * padded_w * 4,
                                static_cast<std::size_t>(width) * 4);
                }
            }

            return image;
        }
        else
        {
            throw std::runtime_error("Unsupported CompressedBG version: " + std::to_string(version));
        }
    }

    void EncodeCbg(const RasterImage &image, const std::filesystem::path &path)
    {
        if (image.width == 0 || image.height == 0 || image.pixels.size() != static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 4)
        {
            throw std::runtime_error("RasterImage is invalid.");
        }
        if (image.width > std::numeric_limits<std::uint16_t>::max() || image.height > std::numeric_limits<std::uint16_t>::max())
        {
            throw std::runtime_error("CBG1 only supports dimensions up to 65535x65535.");
        }

        bool has_real_alpha = false;
        for (std::size_t i = 3; i < image.pixels.size(); i += 4)
        {
            if (image.pixels[i] != 0xFF)
            {
                has_real_alpha = true;
                break;
            }
        }

        const std::uint32_t channels = has_real_alpha ? 4u : 3u;
        auto packed_pixels = PackPixels(image, channels);
        ApplyAverageSampling(packed_pixels, image.width, image.height, channels);
        const auto rle_data = EncodeRle(packed_pixels);

        std::array<std::uint32_t, 256> frequencies{};
        for (const auto value : rle_data)
        {
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

} // namespace hazuki
