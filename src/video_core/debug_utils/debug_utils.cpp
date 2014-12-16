// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2
// Refer to the license.txt file included.

#include <algorithm>
#include <condition_variable>
#include <list>
#include <map>
#include <fstream>
#include <mutex>
#include <string>

#ifdef HAVE_PNG
#include <png.h>
#endif

#include "common/log.h"
#include "common/file_util.h"

#include "video_core/math.h"
#include "video_core/pica.h"

#include "debug_utils.h"

namespace Pica {

void DebugContext::OnEvent(Event event, void* data) {
    if (!breakpoints[event].enabled)
        return;

    {
        std::unique_lock<std::mutex> lock(breakpoint_mutex);

        // TODO: Should stop the CPU thread here once we multithread emulation.

        active_breakpoint = event;
        at_breakpoint = true;

        // Tell all observers that we hit a breakpoint
        for (auto& breakpoint_observer : breakpoint_observers) {
            breakpoint_observer->OnPicaBreakPointHit(event, data);
        }

        // Wait until another thread tells us to Resume()
        resume_from_breakpoint.wait(lock, [&]{ return !at_breakpoint; });
    }
}

void DebugContext::Resume() {
    {
        std::unique_lock<std::mutex> lock(breakpoint_mutex);

        // Tell all observers that we are about to resume
        for (auto& breakpoint_observer : breakpoint_observers) {
            breakpoint_observer->OnPicaResume();
        }

        // Resume the waiting thread (i.e. OnEvent())
        at_breakpoint = false;
    }

    resume_from_breakpoint.notify_one();
}

std::shared_ptr<DebugContext> g_debug_context; // TODO: Get rid of this global

namespace DebugUtils {

void GeometryDumper::AddTriangle(Vertex& v0, Vertex& v1, Vertex& v2) {
    vertices.push_back(v0);
    vertices.push_back(v1);
    vertices.push_back(v2);

    int num_vertices = vertices.size();
    faces.push_back({ num_vertices-3, num_vertices-2, num_vertices-1 });
}

void GeometryDumper::Dump() {
    // NOTE: Permanently enabling this just trashes the hard disk for no reason.
    //       Hence, this is currently disabled.
    return;

    static int index = 0;
    std::string filename = std::string("geometry_dump") + std::to_string(++index) + ".obj";

    std::ofstream file(filename);

    for (const auto& vertex : vertices) {
        file << "v " << vertex.pos[0]
             << " "  << vertex.pos[1]
             << " "  << vertex.pos[2] << std::endl;
    }

    for (const Face& face : faces) {
        file << "f " << 1+face.index[0]
             << " "  << 1+face.index[1]
             << " "  << 1+face.index[2] << std::endl;
    }
}

#pragma pack(1)
struct DVLBHeader {
    enum : u32 {
        MAGIC_WORD = 0x424C5644, // "DVLB"
    };

    u32 magic_word;
    u32 num_programs;
//    u32 dvle_offset_table[];
};
static_assert(sizeof(DVLBHeader) == 0x8, "Incorrect structure size");

struct DVLPHeader {
    enum : u32 {
        MAGIC_WORD = 0x504C5644, // "DVLP"
    };

    u32 magic_word;
    u32 version;
    u32 binary_offset;  // relative to DVLP start
    u32 binary_size_words;
    u32 swizzle_patterns_offset;
    u32 swizzle_patterns_num_entries;
    u32 unk2;
};
static_assert(sizeof(DVLPHeader) == 0x1C, "Incorrect structure size");

struct DVLEHeader {
    enum : u32 {
        MAGIC_WORD = 0x454c5644, // "DVLE"
    };

    enum class ShaderType : u8 {
        VERTEX = 0,
        GEOMETRY = 1,
    };

    u32 magic_word;
    u16 pad1;
    ShaderType type;
    u8 pad2;
    u32 main_offset_words; // offset within binary blob
    u32 endmain_offset_words;
    u32 pad3;
    u32 pad4;
    u32 constant_table_offset;
    u32 constant_table_size; // number of entries
    u32 label_table_offset;
    u32 label_table_size;
    u32 output_register_table_offset;
    u32 output_register_table_size;
    u32 uniform_table_offset;
    u32 uniform_table_size;
    u32 symbol_table_offset;
    u32 symbol_table_size;

};
static_assert(sizeof(DVLEHeader) == 0x40, "Incorrect structure size");
#pragma pack()

void DumpShader(const u32* binary_data, u32 binary_size, const u32* swizzle_data, u32 swizzle_size,
                u32 main_offset, const Regs::VSOutputAttributes* output_attributes)
{
    // NOTE: Permanently enabling this just trashes hard disks for no reason.
    //       Hence, this is currently disabled.
    return;

    struct StuffToWrite {
        u8* pointer;
        u32 size;
    };
    std::vector<StuffToWrite> writing_queue;
    u32 write_offset = 0;

    auto QueueForWriting = [&writing_queue,&write_offset](u8* pointer, u32 size) {
        writing_queue.push_back({pointer, size});
        u32 old_write_offset = write_offset;
        write_offset += size;
        return old_write_offset;
    };

    // First off, try to translate Pica state (one enum for output attribute type and component)
    // into shbin format (separate type and component mask).
    union OutputRegisterInfo {
        enum Type : u64 {
            POSITION = 0,
            COLOR = 2,
            TEXCOORD0 = 3,
            TEXCOORD1 = 5,
            TEXCOORD2 = 6,
        };

        BitField< 0, 64, u64> hex;

        BitField< 0, 16, Type> type;
        BitField<16, 16, u64> id;
        BitField<32,  4, u64> component_mask;
    };

    // This is put into a try-catch block to make sure we notice unknown configurations.
    std::vector<OutputRegisterInfo> output_info_table;
        for (unsigned i = 0; i < 7; ++i) {
            using OutputAttributes = Pica::Regs::VSOutputAttributes;

            // TODO: It's still unclear how the attribute components map to the register!
            //       Once we know that, this code probably will not make much sense anymore.
            std::map<OutputAttributes::Semantic, std::pair<OutputRegisterInfo::Type, u32> > map = {
                { OutputAttributes::POSITION_X, { OutputRegisterInfo::POSITION, 1} },
                { OutputAttributes::POSITION_Y, { OutputRegisterInfo::POSITION, 2} },
                { OutputAttributes::POSITION_Z, { OutputRegisterInfo::POSITION, 4} },
                { OutputAttributes::POSITION_W, { OutputRegisterInfo::POSITION, 8} },
                { OutputAttributes::COLOR_R, { OutputRegisterInfo::COLOR, 1} },
                { OutputAttributes::COLOR_G, { OutputRegisterInfo::COLOR, 2} },
                { OutputAttributes::COLOR_B, { OutputRegisterInfo::COLOR, 4} },
                { OutputAttributes::COLOR_A, { OutputRegisterInfo::COLOR, 8} },
                { OutputAttributes::TEXCOORD0_U, { OutputRegisterInfo::TEXCOORD0, 1} },
                { OutputAttributes::TEXCOORD0_V, { OutputRegisterInfo::TEXCOORD0, 2} },
                { OutputAttributes::TEXCOORD1_U, { OutputRegisterInfo::TEXCOORD1, 1} },
                { OutputAttributes::TEXCOORD1_V, { OutputRegisterInfo::TEXCOORD1, 2} },
                { OutputAttributes::TEXCOORD2_U, { OutputRegisterInfo::TEXCOORD2, 1} },
                { OutputAttributes::TEXCOORD2_V, { OutputRegisterInfo::TEXCOORD2, 2} }
            };

            for (const auto& semantic : std::vector<OutputAttributes::Semantic>{
                                                output_attributes[i].map_x,
                                                output_attributes[i].map_y,
                                                output_attributes[i].map_z,
                                                output_attributes[i].map_w     }) {
                if (semantic == OutputAttributes::INVALID)
                    continue;

                try {
                    OutputRegisterInfo::Type type = map.at(semantic).first;
                    u32 component_mask = map.at(semantic).second;

                    auto it = std::find_if(output_info_table.begin(), output_info_table.end(),
                                        [&i, &type](const OutputRegisterInfo& info) {
                                            return info.id == i && info.type == type;
                                        }
                                        );

                    if (it == output_info_table.end()) {
                        output_info_table.push_back({});
                        output_info_table.back().type = type;
                        output_info_table.back().component_mask = component_mask;
                        output_info_table.back().id = i;
                    } else {
                        it->component_mask = it->component_mask | component_mask;
                    }
                } catch (const std::out_of_range& ) {
                    _dbg_assert_msg_(HW_GPU, 0, "Unknown output attribute mapping");
                    LOG_ERROR(HW_GPU, "Unknown output attribute mapping: %03x, %03x, %03x, %03x",
                              (int)output_attributes[i].map_x.Value(),
                              (int)output_attributes[i].map_y.Value(),
                              (int)output_attributes[i].map_z.Value(),
                              (int)output_attributes[i].map_w.Value());
                }
            }
        }


    struct {
        DVLBHeader header;
        u32 dvle_offset;
    } dvlb{ {DVLBHeader::MAGIC_WORD, 1 } }; // 1 DVLE

    DVLPHeader dvlp{ DVLPHeader::MAGIC_WORD };
    DVLEHeader dvle{ DVLEHeader::MAGIC_WORD };

    QueueForWriting((u8*)&dvlb, sizeof(dvlb));
    u32 dvlp_offset = QueueForWriting((u8*)&dvlp, sizeof(dvlp));
    dvlb.dvle_offset = QueueForWriting((u8*)&dvle, sizeof(dvle));

    // TODO: Reduce the amount of binary code written to relevant portions
    dvlp.binary_offset = write_offset - dvlp_offset;
    dvlp.binary_size_words = binary_size;
    QueueForWriting((u8*)binary_data, binary_size * sizeof(u32));

    dvlp.swizzle_patterns_offset = write_offset - dvlp_offset;
    dvlp.swizzle_patterns_num_entries = swizzle_size;
    u32 dummy = 0;
    for (unsigned int i = 0; i < swizzle_size; ++i) {
        QueueForWriting((u8*)&swizzle_data[i], sizeof(swizzle_data[i]));
        QueueForWriting((u8*)&dummy, sizeof(dummy));
    }

    dvle.main_offset_words = main_offset;
    dvle.output_register_table_offset = write_offset - dvlb.dvle_offset;
    dvle.output_register_table_size = output_info_table.size();
    QueueForWriting((u8*)output_info_table.data(), output_info_table.size() * sizeof(OutputRegisterInfo));

    // TODO: Create a label table for "main"


    // Write data to file
    static int dump_index = 0;
    std::string filename = std::string("shader_dump") + std::to_string(++dump_index) + std::string(".shbin");
    std::ofstream file(filename, std::ios_base::out | std::ios_base::binary);

    for (auto& chunk : writing_queue) {
        file.write((char*)chunk.pointer, chunk.size);
    }
}

static std::unique_ptr<PicaTrace> pica_trace;
static std::mutex pica_trace_mutex;
static int is_pica_tracing = false;

void StartPicaTracing()
{
    if (is_pica_tracing) {
        LOG_WARNING(HW_GPU, "StartPicaTracing called even though tracing already running!");
        return;
    }

    pica_trace_mutex.lock();
    pica_trace = std::unique_ptr<PicaTrace>(new PicaTrace);

    is_pica_tracing = true;
    pica_trace_mutex.unlock();
}

bool IsPicaTracing()
{
    return is_pica_tracing != 0;
}

void OnPicaRegWrite(u32 id, u32 value)
{
    // Double check for is_pica_tracing to avoid pointless locking overhead
    if (!is_pica_tracing)
        return;

    std::unique_lock<std::mutex> lock(pica_trace_mutex);

    if (!is_pica_tracing)
        return;

    pica_trace->writes.push_back({id, value});
}

std::unique_ptr<PicaTrace> FinishPicaTracing()
{
    if (!is_pica_tracing) {
        LOG_WARNING(HW_GPU, "FinishPicaTracing called even though tracing isn't running!");
        return {};
    }

    // signalize that no further tracing should be performed
    is_pica_tracing = false;

    // Wait until running tracing is finished
    pica_trace_mutex.lock();
    std::unique_ptr<PicaTrace> ret(std::move(pica_trace));
    pica_trace_mutex.unlock();
    return std::move(ret);
}

const Math::Vec4<u8> LookupTexture(const u8* source, int x, int y, const TextureInfo& info, bool disable_alpha) {

    // Images are split into 8x8 tiles. Each tile is composed of four 4x4 subtiles each
    // of which is composed of four 2x2 subtiles each of which is composed of four texels.
    // Each structure is embedded into the next-bigger one in a diagonal pattern, e.g.
    // texels are laid out in a 2x2 subtile like this:
    // 2 3
    // 0 1
    //
    // The full 8x8 tile has the texels arranged like this:
    //
    // 42 43 46 47 58 59 62 63
    // 40 41 44 45 56 57 60 61
    // 34 35 38 39 50 51 54 55
    // 32 33 36 37 48 49 52 53
    // 10 11 14 15 26 27 30 31
    // 08 09 12 13 24 25 28 29
    // 02 03 06 07 18 19 22 23
    // 00 01 04 05 16 17 20 21

    // TODO(neobrain): Not sure if this swizzling pattern is used for all textures.
    // To be flexible in case different but similar patterns are used, we keep this
    // somewhat inefficient code around for now.
    int texel_index_within_tile = 0;
    for (int block_size_index = 0; block_size_index < 3; ++block_size_index) {
        int sub_tile_width = 1 << block_size_index;
        int sub_tile_height = 1 << block_size_index;

        int sub_tile_index = (x & sub_tile_width) << block_size_index;
        sub_tile_index += 2 * ((y & sub_tile_height) << block_size_index);
        texel_index_within_tile += sub_tile_index;
    }

    const int block_width = 8;
    const int block_height = 8;

    int coarse_x = (x / block_width) * block_width;
    int coarse_y = (y / block_height) * block_height;

    switch (info.format) {
    case Regs::TextureFormat::RGBA8:
    {
        const u8* source_ptr = source + coarse_x * block_height * 4 + coarse_y * info.stride + texel_index_within_tile * 4;
        return { source_ptr[3], source_ptr[2], source_ptr[1], disable_alpha ? 255 : source_ptr[0] };
    }

    case Regs::TextureFormat::RGB8:
    {
        const u8* source_ptr = source + coarse_x * block_height * 3 + coarse_y * info.stride + texel_index_within_tile * 3;
        return { source_ptr[2], source_ptr[1], source_ptr[0], 255 };
    }

    case Regs::TextureFormat::RGBA5551:
    {
        const u16 source_ptr = *(const u16*)(source + coarse_x * block_height * 2 + coarse_y * info.stride + texel_index_within_tile * 2);
        u8 r = (source_ptr >> 11) & 0x1F;
        u8 g = ((source_ptr) >> 6) & 0x1F;
        u8 b = (source_ptr >> 1) & 0x1F;
        u8 a = source_ptr & 1;
        return Math::MakeVec<u8>((r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2), disable_alpha ? 255 : (a * 255));
    }

    case Regs::TextureFormat::RGBA4:
    {
        const u8* source_ptr = source + coarse_x * block_height * 2 + coarse_y * info.stride + texel_index_within_tile * 2;
        u8 r = source_ptr[1] >> 4;
        u8 g = source_ptr[1] & 0xFF;
        u8 b = source_ptr[0] >> 4;
        u8 a = source_ptr[0] & 0xFF;
        r = (r << 4) | r;
        g = (g << 4) | g;
        b = (b << 4) | b;
        a = (a << 4) | a;
        return { r, g, b, disable_alpha ? 255 : a };
    }

    case Regs::TextureFormat::A8:
    {
        const u8* source_ptr = source + coarse_x * block_height + coarse_y * info.stride + texel_index_within_tile;

        // TODO: Better control this...
        if (disable_alpha) {
            return { *source_ptr, *source_ptr, *source_ptr, 255 };
        } else {
            return { 0, 0, 0, *source_ptr };
        }
    }

    default:
        LOG_ERROR(HW_GPU, "Unknown texture format: %x", (u32)info.format);
        _dbg_assert_(HW_GPU, 0);
        return {};
    }
}

TextureInfo TextureInfo::FromPicaRegister(const Regs::TextureConfig& config,
                                          const Regs::TextureFormat& format)
{
    TextureInfo info;
    info.address = config.GetPhysicalAddress();
    info.width = config.width;
    info.height = config.height;
    info.format = format;
    info.stride = Pica::Regs::BytesPerPixel(info.format) * info.width;
    return info;
}

void DumpTexture(const Pica::Regs::TextureConfig& texture_config, u8* data) {
    // NOTE: Permanently enabling this just trashes hard disks for no reason.
    //       Hence, this is currently disabled.
    return;

#ifndef HAVE_PNG
    return;
#else
    if (!data)
        return;

    // Write data to file
    static int dump_index = 0;
    std::string filename = std::string("texture_dump") + std::to_string(++dump_index) + std::string(".png");
    u32 row_stride = texture_config.width * 3;

    u8* buf;

    char title[] = "Citra texture dump";
    char title_key[] = "Title";
    png_structp png_ptr = nullptr;
    png_infop info_ptr = nullptr;

    // Open file for writing (binary mode)
    FileUtil::IOFile fp(filename, "wb");

    // Initialize write structure
    png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (png_ptr == nullptr) {
        LOG_ERROR(Debug_GPU, "Could not allocate write struct\n");
        goto finalise;

    }

    // Initialize info structure
    info_ptr = png_create_info_struct(png_ptr);
    if (info_ptr == nullptr) {
        LOG_ERROR(Debug_GPU, "Could not allocate info struct\n");
        goto finalise;
    }

    // Setup Exception handling
    if (setjmp(png_jmpbuf(png_ptr))) {
        LOG_ERROR(Debug_GPU, "Error during png creation\n");
        goto finalise;
    }

    png_init_io(png_ptr, fp.GetHandle());

    // Write header (8 bit colour depth)
    png_set_IHDR(png_ptr, info_ptr, texture_config.width, texture_config.height,
        8, PNG_COLOR_TYPE_RGB /*_ALPHA*/, PNG_INTERLACE_NONE,
        PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);

    png_text title_text;
    title_text.compression = PNG_TEXT_COMPRESSION_NONE;
    title_text.key = title_key;
    title_text.text = title;
    png_set_text(png_ptr, info_ptr, &title_text, 1);

    png_write_info(png_ptr, info_ptr);

    buf = new u8[row_stride * texture_config.height];
    for (unsigned y = 0; y < texture_config.height; ++y) {
        for (unsigned x = 0; x < texture_config.width; ++x) {
            TextureInfo info;
            info.width = texture_config.width;
            info.height = texture_config.height;
            info.stride = row_stride;
            info.format = registers.texture0_format;
            Math::Vec4<u8> texture_color = LookupTexture(data, x, y, info);
            buf[3 * x + y * row_stride    ] = texture_color.r();
            buf[3 * x + y * row_stride + 1] = texture_color.g();
            buf[3 * x + y * row_stride + 2] = texture_color.b();
        }
    }

    // Write image data
    for (unsigned y = 0; y < texture_config.height; ++y)
    {
        u8* row_ptr = (u8*)buf + y * row_stride;
        u8* ptr = row_ptr;
        png_write_row(png_ptr, row_ptr);
    }

    delete[] buf;

    // End write
    png_write_end(png_ptr, nullptr);

finalise:
    if (info_ptr != nullptr) png_free_data(png_ptr, info_ptr, PNG_FREE_ALL, -1);
    if (png_ptr != nullptr) png_destroy_write_struct(&png_ptr, (png_infopp)nullptr);
#endif
}

void DumpTevStageConfig(const std::array<Pica::Regs::TevStageConfig,6>& stages)
{
    using Source = Pica::Regs::TevStageConfig::Source;
    using ColorModifier = Pica::Regs::TevStageConfig::ColorModifier;
    using AlphaModifier = Pica::Regs::TevStageConfig::AlphaModifier;
    using Operation = Pica::Regs::TevStageConfig::Operation;

    std::string stage_info = "Tev setup:\n";
    for (size_t index = 0; index < stages.size(); ++index) {
        const auto& tev_stage = stages[index];

        const std::map<Source, std::string> source_map = {
            { Source::PrimaryColor, "PrimaryColor" },
            { Source::Texture0, "Texture0" },
            { Source::Constant, "Constant" },
            { Source::Previous, "Previous" },
        };

        const std::map<ColorModifier, std::string> color_modifier_map = {
            { ColorModifier::SourceColor, { "%source.rgb" } }
        };
        const std::map<AlphaModifier, std::string> alpha_modifier_map = {
            { AlphaModifier::SourceAlpha, "%source.a" }
        };

        std::map<Operation, std::string> combiner_map = {
            { Operation::Replace, "%source1" },
            { Operation::Modulate, "(%source1 * %source2) / 255" },
        };

        auto ReplacePattern =
                [](const std::string& input, const std::string& pattern, const std::string& replacement) -> std::string {
                    size_t start = input.find(pattern);
                    if (start == std::string::npos)
                        return input;

                    std::string ret = input;
                    ret.replace(start, pattern.length(), replacement);
                    return ret;
                };
        auto GetColorSourceStr =
                [&source_map,&color_modifier_map,&ReplacePattern](const Source& src, const ColorModifier& modifier) {
                    auto src_it = source_map.find(src);
                    std::string src_str = "Unknown";
                    if (src_it != source_map.end())
                        src_str = src_it->second;

                    auto modifier_it = color_modifier_map.find(modifier);
                    std::string modifier_str = "%source.????";
                    if (modifier_it != color_modifier_map.end())
                        modifier_str = modifier_it->second;

                    return ReplacePattern(modifier_str, "%source", src_str);
                };
        auto GetColorCombinerStr =
                [&](const Regs::TevStageConfig& tev_stage) {
                    auto op_it = combiner_map.find(tev_stage.color_op);
                    std::string op_str = "Unknown op (%source1, %source2, %source3)";
                    if (op_it != combiner_map.end())
                        op_str = op_it->second;

                    op_str = ReplacePattern(op_str, "%source1", GetColorSourceStr(tev_stage.color_source1, tev_stage.color_modifier1));
                    op_str = ReplacePattern(op_str, "%source2", GetColorSourceStr(tev_stage.color_source2, tev_stage.color_modifier2));
                    return   ReplacePattern(op_str, "%source3", GetColorSourceStr(tev_stage.color_source3, tev_stage.color_modifier3));
                };
        auto GetAlphaSourceStr =
                [&source_map,&alpha_modifier_map,&ReplacePattern](const Source& src, const AlphaModifier& modifier) {
                    auto src_it = source_map.find(src);
                    std::string src_str = "Unknown";
                    if (src_it != source_map.end())
                        src_str = src_it->second;

                    auto modifier_it = alpha_modifier_map.find(modifier);
                    std::string modifier_str = "%source.????";
                    if (modifier_it != alpha_modifier_map.end())
                        modifier_str = modifier_it->second;

                    return ReplacePattern(modifier_str, "%source", src_str);
                };
        auto GetAlphaCombinerStr =
                [&](const Regs::TevStageConfig& tev_stage) {
                    auto op_it = combiner_map.find(tev_stage.alpha_op);
                    std::string op_str = "Unknown op (%source1, %source2, %source3)";
                    if (op_it != combiner_map.end())
                        op_str = op_it->second;

                    op_str = ReplacePattern(op_str, "%source1", GetAlphaSourceStr(tev_stage.alpha_source1, tev_stage.alpha_modifier1));
                    op_str = ReplacePattern(op_str, "%source2", GetAlphaSourceStr(tev_stage.alpha_source2, tev_stage.alpha_modifier2));
                    return   ReplacePattern(op_str, "%source3", GetAlphaSourceStr(tev_stage.alpha_source3, tev_stage.alpha_modifier3));
                };

        stage_info += "Stage " + std::to_string(index) + ": " + GetColorCombinerStr(tev_stage) + "   " + GetAlphaCombinerStr(tev_stage) + "\n";
    }

    LOG_TRACE(HW_GPU, "%s", stage_info.c_str());
}

} // namespace

} // namespace
