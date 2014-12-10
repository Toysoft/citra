// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2
// Refer to the license.txt file included.

#include <algorithm>

#include "common/common_types.h"

#include "math.h"
#include "pica.h"
#include "rasterizer.h"
#include "vertex_shader.h"

#include "debug_utils/debug_utils.h"

namespace Pica {

namespace Rasterizer {

static void DrawPixel(int x, int y, const Math::Vec4<u8>& color) {
    u32* color_buffer = (u32*)Memory::GetPointer(registers.framebuffer.GetColorBufferAddress());
    u32 value = (color.a() << 24) | (color.r() << 16) | (color.g() << 8) | color.b();

    // Assuming RGBA8 format until actual framebuffer format handling is implemented
    *(color_buffer + x + y * registers.framebuffer.GetWidth()) = value;
}

static u32 GetDepth(int x, int y) {
    u16* depth_buffer = (u16*)Memory::GetPointer(registers.framebuffer.GetDepthBufferAddress());

    // Assuming 16-bit depth buffer format until actual format handling is implemented
    return *(depth_buffer + x + y * registers.framebuffer.GetWidth());
}

static void SetDepth(int x, int y, u16 value) {
    u16* depth_buffer = (u16*)Memory::GetPointer(registers.framebuffer.GetDepthBufferAddress());

    // Assuming 16-bit depth buffer format until actual format handling is implemented
    *(depth_buffer + x + y * registers.framebuffer.GetWidth()) = value;
}

void ProcessTriangle(const VertexShader::OutputVertex& v0,
                     const VertexShader::OutputVertex& v1,
                     const VertexShader::OutputVertex& v2)
{
    // NOTE: Assuming that rasterizer coordinates are 12.4 fixed-point values
    struct Fix12P4 {
        Fix12P4() {}
        Fix12P4(u16 val) : val(val) {}

        static u16 FracMask() { return 0xF; }
        static u16 IntMask() { return (u16)~0xF; }

        operator u16() const {
            return val;
        }

        bool operator < (const Fix12P4& oth) const {
            return (u16)*this < (u16)oth;
        }

    private:
        u16 val;
    };

    // vertex positions in rasterizer coordinates
    auto FloatToFix = [](float24 flt) {
                          return Fix12P4(static_cast<unsigned short>(flt.ToFloat32() * 16.0f));
                      };
    auto ScreenToRasterizerCoordinates = [FloatToFix](const Math::Vec3<float24> vec) {
                                             return Math::Vec3<Fix12P4>{FloatToFix(vec.x), FloatToFix(vec.y), FloatToFix(vec.z)};
                                         };
    Math::Vec3<Fix12P4> vtxpos[3]{ ScreenToRasterizerCoordinates(v0.screenpos),
                                   ScreenToRasterizerCoordinates(v1.screenpos),
                                   ScreenToRasterizerCoordinates(v2.screenpos) };

    // TODO: Proper scissor rect test!
    u16 min_x = std::min({vtxpos[0].x, vtxpos[1].x, vtxpos[2].x});
    u16 min_y = std::min({vtxpos[0].y, vtxpos[1].y, vtxpos[2].y});
    u16 max_x = std::max({vtxpos[0].x, vtxpos[1].x, vtxpos[2].x});
    u16 max_y = std::max({vtxpos[0].y, vtxpos[1].y, vtxpos[2].y});

    min_x &= Fix12P4::IntMask();
    min_y &= Fix12P4::IntMask();
    max_x = ((max_x + Fix12P4::FracMask()) & Fix12P4::IntMask());
    max_y = ((max_y + Fix12P4::FracMask()) & Fix12P4::IntMask());

    // Triangle filling rules: Pixels on the right-sided edge or on flat bottom edges are not
    // drawn. Pixels on any other triangle border are drawn. This is implemented with three bias
    // values which are added to the barycentric coordinates w0, w1 and w2, respectively.
    // NOTE: These are the PSP filling rules. Not sure if the 3DS uses the same ones...
    auto IsRightSideOrFlatBottomEdge = [](const Math::Vec2<Fix12P4>& vtx,
                                          const Math::Vec2<Fix12P4>& line1,
                                          const Math::Vec2<Fix12P4>& line2)
    {
        if (line1.y == line2.y) {
            // just check if vertex is above us => bottom line parallel to x-axis
            return vtx.y < line1.y;
        } else {
            // check if vertex is on our left => right side
            // TODO: Not sure how likely this is to overflow
            return (int)vtx.x < (int)line1.x + ((int)line2.x - (int)line1.x) * ((int)vtx.y - (int)line1.y) / ((int)line2.y - (int)line1.y);
        }
    };
    int bias0 = IsRightSideOrFlatBottomEdge(vtxpos[0].xy(), vtxpos[1].xy(), vtxpos[2].xy()) ? -1 : 0;
    int bias1 = IsRightSideOrFlatBottomEdge(vtxpos[1].xy(), vtxpos[2].xy(), vtxpos[0].xy()) ? -1 : 0;
    int bias2 = IsRightSideOrFlatBottomEdge(vtxpos[2].xy(), vtxpos[0].xy(), vtxpos[1].xy()) ? -1 : 0;

    // TODO: Not sure if looping through x first might be faster
    for (u16 y = min_y; y < max_y; y += 0x10) {
        for (u16 x = min_x; x < max_x; x += 0x10) {

            // Calculate the barycentric coordinates w0, w1 and w2
            auto orient2d = [](const Math::Vec2<Fix12P4>& vtx1,
                               const Math::Vec2<Fix12P4>& vtx2,
                               const Math::Vec2<Fix12P4>& vtx3) {
                const auto vec1 = Math::MakeVec(vtx2 - vtx1, 0);
                const auto vec2 = Math::MakeVec(vtx3 - vtx1, 0);
                // TODO: There is a very small chance this will overflow for sizeof(int) == 4
                return Math::Cross(vec1, vec2).z;
            };

            int w0 = bias0 + orient2d(vtxpos[1].xy(), vtxpos[2].xy(), {x, y});
            int w1 = bias1 + orient2d(vtxpos[2].xy(), vtxpos[0].xy(), {x, y});
            int w2 = bias2 + orient2d(vtxpos[0].xy(), vtxpos[1].xy(), {x, y});
            int wsum = w0 + w1 + w2;

            // If current pixel is not covered by the current primitive
            if (w0 < 0 || w1 < 0 || w2 < 0)
                continue;

            // Perspective correct attribute interpolation:
            // Attribute values cannot be calculated by simple linear interpolation since
            // they are not linear in screen space. For example, when interpolating a
            // texture coordinate across two vertices, something simple like
            //     u = (u0*w0 + u1*w1)/(w0+w1)
            // will not work. However, the attribute value divided by the
            // clipspace w-coordinate (u/w) and and the inverse w-coordinate (1/w) are linear
            // in screenspace. Hence, we can linearly interpolate these two independently and
            // calculate the interpolated attribute by dividing the results.
            // I.e.
            //     u_over_w   = ((u0/v0.pos.w)*w0 + (u1/v1.pos.w)*w1)/(w0+w1)
            //     one_over_w = (( 1/v0.pos.w)*w0 + ( 1/v1.pos.w)*w1)/(w0+w1)
            //     u = u_over_w / one_over_w
            //
            // The generalization to three vertices is straightforward in baricentric coordinates.
            auto GetInterpolatedAttribute = [&](float24 attr0, float24 attr1, float24 attr2) {
                auto attr_over_w = Math::MakeVec(attr0 / v0.pos.w,
                                                 attr1 / v1.pos.w,
                                                 attr2 / v2.pos.w);
                auto w_inverse   = Math::MakeVec(float24::FromFloat32(1.f) / v0.pos.w,
                                                 float24::FromFloat32(1.f) / v1.pos.w,
                                                 float24::FromFloat32(1.f) / v2.pos.w);
                auto baricentric_coordinates = Math::MakeVec(float24::FromFloat32(static_cast<float>(w0)),
                                                             float24::FromFloat32(static_cast<float>(w1)),
                                                             float24::FromFloat32(static_cast<float>(w2)));

                float24 interpolated_attr_over_w = Math::Dot(attr_over_w, baricentric_coordinates);
                float24 interpolated_w_inverse   = Math::Dot(w_inverse,   baricentric_coordinates);
                return interpolated_attr_over_w / interpolated_w_inverse;
            };

            Math::Vec4<u8> primary_color{
                (u8)(GetInterpolatedAttribute(v0.color.r(), v1.color.r(), v2.color.r()).ToFloat32() * 255),
                (u8)(GetInterpolatedAttribute(v0.color.g(), v1.color.g(), v2.color.g()).ToFloat32() * 255),
                (u8)(GetInterpolatedAttribute(v0.color.b(), v1.color.b(), v2.color.b()).ToFloat32() * 255),
                (u8)(GetInterpolatedAttribute(v0.color.a(), v1.color.a(), v2.color.a()).ToFloat32() * 255)
            };

            Math::Vec2<float24> uv[3];
            uv[0].u() = GetInterpolatedAttribute(v0.tc0.u(), v1.tc0.u(), v2.tc0.u());
            uv[0].v() = GetInterpolatedAttribute(v0.tc0.v(), v1.tc0.v(), v2.tc0.v());
            uv[1].u() = GetInterpolatedAttribute(v0.tc1.u(), v1.tc1.u(), v2.tc1.u());
            uv[1].v() = GetInterpolatedAttribute(v0.tc1.v(), v1.tc1.v(), v2.tc1.v());
            uv[2].u() = GetInterpolatedAttribute(v0.tc2.u(), v1.tc2.u(), v2.tc2.u());
            uv[2].v() = GetInterpolatedAttribute(v0.tc2.v(), v1.tc2.v(), v2.tc2.v());

            Math::Vec4<u8> texture_color[3]{};
            for (int i = 0; i < 3; ++i) {
                auto texture = registers.GetTextures()[i];
                if (texture.enabled)
                    continue;

                _dbg_assert_(GPU, 0 != texture.config.address);

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
                int s = (int)(uv[i].u() * float24::FromFloat32(static_cast<float>(texture.config.width))).ToFloat32();
                int t = (int)(uv[i].v() * float24::FromFloat32(static_cast<float>(texture.config.height))).ToFloat32();
                auto GetWrappedTexCoord = [](Regs::TextureConfig::WrapMode mode, int val, unsigned size) {
                    switch (mode) {
                        case Regs::TextureConfig::ClampToEdge:
                            val = std::max(val, 0);
                            val = std::min(val, (int)size - 1);
                            return val;

                        case Regs::TextureConfig::Repeat:
                            return (int)(((unsigned)val) % size);

                        default:
                            ERROR_LOG(GPU, "Unknown texture coordinate wrapping mode %x\n", (int)mode);
                            _dbg_assert_(GPU, 0);
                            return 0;
                    }
                };
                s = GetWrappedTexCoord(registers.texture0.wrap_s, s, registers.texture0.width);
                t = GetWrappedTexCoord(registers.texture0.wrap_t, t, registers.texture0.height);

                int texel_index_within_tile = 0;
                for (int block_size_index = 0; block_size_index < 3; ++block_size_index) {
                    int sub_tile_width = 1 << block_size_index;
                    int sub_tile_height = 1 << block_size_index;

                    int sub_tile_index = (s & sub_tile_width) << block_size_index;
                    sub_tile_index += 2 * ((t & sub_tile_height) << block_size_index);
                    texel_index_within_tile += sub_tile_index;
                }

                const int block_width = 8;
                const int block_height = 8;

                int coarse_s = (s / block_width) * block_width;
                int coarse_t = (t / block_height) * block_height;

                // TODO: This is currently hardcoded for RGB8
                u32* texture_data = (u32*)Memory::GetPointer(texture.config.GetPhysicalAddress());

                const int row_stride = texture.config.width * 3;
                u8* source_ptr = (u8*)texture_data + coarse_s * block_height * 3 + coarse_t * row_stride + texel_index_within_tile * 3;
                texture_color[i].r() = source_ptr[2];
                texture_color[i].g() = source_ptr[1];
                texture_color[i].b() = source_ptr[0];
                texture_color[i].a() = 0xFF;

                DebugUtils::DumpTexture(texture.config, (u8*)texture_data);
            }

            // Texture environment - consists of 6 stages of color and alpha combining.
            //
            // Color combiners take three input color values from some source (e.g. interpolated
            // vertex color, texture color, previous stage, etc), perform some very simple
            // operations on each of them (e.g. inversion) and then calculate the output color
            // with some basic arithmetic. Alpha combiners can be configured separately but work
            // analogously.
            Math::Vec4<u8> combiner_output;
            for (auto tev_stage : registers.GetTevStages()) {
                using Source = Regs::TevStageConfig::Source;
                using ColorModifier = Regs::TevStageConfig::ColorModifier;
                using AlphaModifier = Regs::TevStageConfig::AlphaModifier;
                using Operation = Regs::TevStageConfig::Operation;

                auto GetColorSource = [&](Source source) -> Math::Vec3<u8> {
                    switch (source) {
                    case Source::PrimaryColor:
                        return primary_color.rgb();

                    case Source::Texture0:
                        return texture_color[0].rgb();

                    case Source::Texture1:
                        return texture_color[1].rgb();

                    case Source::Texture2:
                        return texture_color[2].rgb();

                    case Source::Constant:
                        return {tev_stage.const_r, tev_stage.const_g, tev_stage.const_b};

                    case Source::Previous:
                        return combiner_output.rgb();

                    default:
                        ERROR_LOG(GPU, "Unknown color combiner source %d\n", (int)source);
                        return {};
                    }
                };

                auto GetAlphaSource = [&](Source source) -> u8 {
                    switch (source) {
                    case Source::PrimaryColor:
                        return primary_color.a();

                    case Source::Texture0:
                        return texture_color[0].a();

                    case Source::Texture1:
                        return texture_color[1].a();

                    case Source::Texture2:
                        return texture_color[2].a();

                    case Source::Constant:
                        return tev_stage.const_a;

                    case Source::Previous:
                        return combiner_output.a();

                    default:
                        ERROR_LOG(GPU, "Unknown alpha combiner source %d\n", (int)source);
                        return 0;
                    }
                };

                auto GetColorModifier = [](ColorModifier factor, const Math::Vec3<u8>& values) -> Math::Vec3<u8> {
                    switch (factor)
                    {
                    case ColorModifier::SourceColor:
                        return values;

                    case ColorModifier::SourceAlpha:
                        // TODO: Ugh, I messed up here. Need to fix parameters and stuff..
                        return values;

                    default:
                        ERROR_LOG(GPU, "Unknown color factor %d\n", (int)factor);
                        return {};
                    }
                };

                auto GetAlphaModifier = [](AlphaModifier factor, u8 value) -> u8 {
                    switch (factor) {
                    case AlphaModifier::SourceAlpha:
                        return value;

					case AlphaModifier::OneMinusSourceAlpha:
						return 255 - value;

                    default:
                        ERROR_LOG(GPU, "Unknown alpha factor %d\n", (int)factor);
                        return 0;
                    }
                };

                auto ColorCombine = [](Operation op, const Math::Vec3<u8> input[3]) -> Math::Vec3<u8> {
                    switch (op) {
                    case Operation::Replace:
                        return input[0];

                    case Operation::Modulate:
                        return ((input[0] * input[1]) / 255).Cast<u8>();

                    case Operation::Add:
                        return (input[0] + input[1]).Cast<u8>();

                    case Operation::Lerp:
                        return ((input[0] * input[2] + input[1] * (Math::MakeVec<u8>(255, 255, 255) - input[2]).Cast<u8>()) / 255).Cast<u8>();

                    default:
                        ERROR_LOG(GPU, "Unknown color combiner operation %d\n", (int)op);
                        return {};
                    }
                };

                auto AlphaCombine = [](Operation op, const std::array<u8,3>& input) -> u8 {
                    switch (op) {
                    case Operation::Replace:
                        return input[0];

                    case Operation::Modulate:
                        return input[0] * input[1] / 255;

                    case Operation::Add:
                        return input[0] + input[1];

                    case Operation::Lerp:
                        return (input[0] * input[2] + input[1] * (255 - input[2])) / 255;

                    default:
                        ERROR_LOG(GPU, "Unknown alpha combiner operation %d\n", (int)op);
                        return 0;
                    }
                };

                // color combiner
                // NOTE: Not sure if the alpha combiner might use the color output of the previous
                //       stage as input. Hence, we currently don't directly write the result to
                //       combiner_output.rgb(), but instead store it in a temporary variable until
                //       alpha combining has been done.
                Math::Vec3<u8> color_result[3] = {
                    GetColorModifier(tev_stage.color_modifier1, GetColorSource(tev_stage.color_source1)),
                    GetColorModifier(tev_stage.color_modifier2, GetColorSource(tev_stage.color_source2)),
                    GetColorModifier(tev_stage.color_modifier3, GetColorSource(tev_stage.color_source3))
                };
                auto color_output = ColorCombine(tev_stage.color_op, color_result);

                // alpha combiner
                std::array<u8,3> alpha_result = {
                    GetAlphaModifier(tev_stage.alpha_modifier1, GetAlphaSource(tev_stage.alpha_source1)),
                    GetAlphaModifier(tev_stage.alpha_modifier2, GetAlphaSource(tev_stage.alpha_source2)),
                    GetAlphaModifier(tev_stage.alpha_modifier3, GetAlphaSource(tev_stage.alpha_source3))
                };
                auto alpha_output = AlphaCombine(tev_stage.alpha_op, alpha_result);

                combiner_output = Math::MakeVec(color_output, alpha_output);
            }

            // TODO: Not sure if the multiplication by 65535 has already been taken care
            // of when transforming to screen coordinates or not.
            u16 z = (u16)(((float)v0.screenpos[2].ToFloat32() * w0 +
                           (float)v1.screenpos[2].ToFloat32() * w1 +
                           (float)v2.screenpos[2].ToFloat32() * w2) * 65535.f / wsum);
            SetDepth(x >> 4, y >> 4, z);

            DrawPixel(x >> 4, y >> 4, combiner_output);
        }
    }
}

} // namespace Rasterizer

} // namespace Pica
