#include "rive/tess/sokol/sokol_tess_renderer.hpp"
#include "rive/tess/sokol/sokol_factory.hpp"
#include "rive/tess/tess_render_path.hpp"
#include "rive/tess/contour_stroke.hpp"
#include "generated/shader.h"
#include <unordered_set>

using namespace rive;

static void fillColorBuffer(float* buffer, ColorInt value) {
    buffer[0] = (float)colorRed(value) / 0xFF;
    buffer[1] = (float)colorGreen(value) / 0xFF;
    buffer[2] = (float)colorBlue(value) / 0xFF;
    buffer[3] = colorOpacity(value);
}

class SokolRenderPath : public TessRenderPath {
public:
    SokolRenderPath() {}

    ~SokolRenderPath() {
        sg_destroy_buffer(m_vertexBuffer);
        sg_destroy_buffer(m_indexBuffer);
    }

private:
    std::vector<Vec2D> m_vertices;
    std::vector<uint16_t> m_indices;

    sg_buffer m_vertexBuffer = {0};
    sg_buffer m_indexBuffer = {0};

    std::size_t m_boundsIndex = 0;

protected:
    void addTriangles(rive::Span<const rive::Vec2D> vts, rive::Span<const uint16_t> idx) override {
        m_vertices.insert(m_vertices.end(), vts.begin(), vts.end());
        m_indices.insert(m_indices.end(), idx.begin(), idx.end());
    }

    void setTriangulatedBounds(const AABB& value) override {
        m_boundsIndex = m_vertices.size();
        m_vertices.emplace_back(Vec2D(value.minX, value.minY));
        m_vertices.emplace_back(Vec2D(value.maxX, value.minY));
        m_vertices.emplace_back(Vec2D(value.maxX, value.maxY));
        m_vertices.emplace_back(Vec2D(value.minX, value.maxY));
    }

public:
    void reset() override {
        TessRenderPath::reset();
        m_vertices.clear();
        m_indices.clear();
    }

    void drawStroke(ContourStroke* stroke) {
        if (isContainer()) {
            for (auto& subPath : m_subPaths) {
                reinterpret_cast<SokolRenderPath*>(subPath.path())->drawStroke(stroke);
            }
            return;
        }
        std::size_t start, end;
        stroke->nextRenderOffset(start, end);
        sg_draw(start < 2 ? 0 : (start - 2) * 3, end - start < 2 ? 0 : (end - start - 2) * 3, 1);
    }

    void drawFill() {
        if (triangulate()) {
            sg_destroy_buffer(m_vertexBuffer);
            sg_destroy_buffer(m_indexBuffer);
            if (m_indices.size() == 0 || m_vertices.size() == 0) {
                m_vertexBuffer = {0};
                m_indexBuffer = {0};
                return;
            }

            m_vertexBuffer = sg_make_buffer((sg_buffer_desc){
                .type = SG_BUFFERTYPE_VERTEXBUFFER,
                .data =
                    {

                        m_vertices.data(),
                        m_vertices.size() * sizeof(Vec2D),
                    },
            });

            m_indexBuffer = sg_make_buffer((sg_buffer_desc){
                .type = SG_BUFFERTYPE_INDEXBUFFER,
                .data =
                    {
                        m_indices.data(),
                        m_indices.size() * sizeof(uint16_t),
                    },
            });
        }

        if (m_vertexBuffer.id == 0) {
            return;
        }

        sg_bindings bind = {
            .vertex_buffers[0] = m_vertexBuffer,
            .index_buffer = m_indexBuffer,
        };

        sg_apply_bindings(&bind);
        sg_draw(0, m_indices.size(), 1);
    }

    void drawBounds(const sg_buffer& boundsIndexBuffer) {
        if (m_vertexBuffer.id == 0) {
            return;
        }
        sg_bindings bind = {
            .vertex_buffers[0] = m_vertexBuffer,
            .vertex_buffer_offsets[0] = (int)(m_boundsIndex * sizeof(Vec2D)),
            .index_buffer = boundsIndexBuffer,
        };

        sg_apply_bindings(&bind);
        sg_draw(0, 6, 1);
    }
};

// Returns a full-formed RenderPath -- can be treated as immutable
std::unique_ptr<RenderPath> SokolFactory::makeRenderPath(Span<const Vec2D> points,
                                                         Span<const PathVerb> verbs,
                                                         FillRule) {
    return std::make_unique<SokolRenderPath>();
}

std::unique_ptr<RenderPath> SokolFactory::makeEmptyRenderPath() {
    return std::make_unique<SokolRenderPath>();
}

SokolRenderImage::SokolRenderImage(sg_image image) : m_image(image) {}

class SokolBuffer : public RenderBuffer {
private:
    sg_buffer m_Buffer;

public:
    SokolBuffer(size_t count, const sg_buffer_desc& desc) :
        RenderBuffer(count), m_Buffer(sg_make_buffer(desc)) {}
    ~SokolBuffer() { sg_destroy_buffer(m_Buffer); }

    sg_buffer buffer() { return m_Buffer; }
};

rcp<RenderBuffer> SokolFactory::makeBufferU16(Span<const uint16_t> span) {
    return rcp<RenderBuffer>(new SokolBuffer(span.size(),
                                             (sg_buffer_desc){
                                                 .type = SG_BUFFERTYPE_INDEXBUFFER,
                                                 .data =
                                                     {
                                                         span.data(),
                                                         span.size_bytes(),
                                                     },
                                             }));
}

rcp<RenderBuffer> SokolFactory::makeBufferU32(Span<const uint32_t> span) {
    return rcp<RenderBuffer>(new SokolBuffer(span.size(),
                                             (sg_buffer_desc){
                                                 .type = SG_BUFFERTYPE_INDEXBUFFER,
                                                 .data =
                                                     {
                                                         span.data(),
                                                         span.size_bytes(),
                                                     },
                                             }));
}
rcp<RenderBuffer> SokolFactory::makeBufferF32(Span<const float> span) {
    return rcp<RenderBuffer>(new SokolBuffer(span.size(),
                                             (sg_buffer_desc){
                                                 .type = SG_BUFFERTYPE_VERTEXBUFFER,
                                                 .data =
                                                     {
                                                         span.data(),
                                                         span.size_bytes(),
                                                     },
                                             }));
}

sg_pipeline vectorPipeline(sg_shader shader,
                           sg_blend_state blend,
                           sg_stencil_state stencil,
                           sg_color_mask colorMask = SG_COLORMASK_RGBA) {
    return sg_make_pipeline((sg_pipeline_desc){
        .layout =
            {
                .attrs =
                    {
                        [ATTR_vs_path_position] =
                            {
                                .format = SG_VERTEXFORMAT_FLOAT2,
                                .buffer_index = 0,
                            },
                    },
            },
        .shader = shader,
        .index_type = SG_INDEXTYPE_UINT16,
        .cull_mode = SG_CULLMODE_NONE,
        .depth =
            {
                .compare = SG_COMPAREFUNC_ALWAYS,
                .write_enabled = false,
            },
        .colors =
            {
                [0] =
                    {
                        .write_mask = colorMask,
                        .blend = blend,
                    },
            },
        .stencil = stencil,
        .label = "path-pipeline",
    });
}

SokolTessRenderer::SokolTessRenderer() {
    m_meshPipeline = sg_make_pipeline((sg_pipeline_desc){
        .layout =
            {
                .attrs =
                    {
                        [ATTR_vs_position] = {.format = SG_VERTEXFORMAT_FLOAT2, .buffer_index = 0},
                        [ATTR_vs_texcoord0] = {.format = SG_VERTEXFORMAT_FLOAT2, .buffer_index = 1},
                    },
            },
        .shader = sg_make_shader(rive_tess_shader_desc(sg_query_backend())),
        .index_type = SG_INDEXTYPE_UINT16,
        .cull_mode = SG_CULLMODE_NONE,
        .depth =
            {
                .compare = SG_COMPAREFUNC_ALWAYS,
                .write_enabled = false,
            },
        .colors =
            {
                [0] =
                    {
                        .write_mask = SG_COLORMASK_RGBA,
                        .blend =
                            {
                                .enabled = true,
                                .src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA,
                                .dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
                            },
                    },
            },
        .label = "mesh-pipeline",
    });

    auto uberShader = sg_make_shader(rive_tess_path_shader_desc(sg_query_backend()));

    assert(maxClippingPaths < 256);

    // Src Over Pipelines
    {
        m_pathPipeline[0] = vectorPipeline(uberShader,
                                           {
                                               .enabled = true,
                                               .src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA,
                                               .dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
                                           },
                                           {
                                               .enabled = false,
                                           });

        for (std::size_t i = 1; i <= maxClippingPaths; i++) {
            m_pathPipeline[i] =
                vectorPipeline(uberShader,
                               {
                                   .enabled = true,
                                   .src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA,
                                   .dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
                               },
                               {
                                   .enabled = true,
                                   .ref = (uint8_t)i,
                                   .read_mask = 0xFF,
                                   .write_mask = 0x00,
                                   .front =
                                       {
                                           .compare = SG_COMPAREFUNC_EQUAL,
                                       },
                                   .back =
                                       {
                                           .compare = SG_COMPAREFUNC_EQUAL,
                                       },
                               });
        }
    }

    // Screen Pipelines
    {
        m_pathScreenPipeline[0] =
            vectorPipeline(uberShader,
                           {
                               .enabled = true,
                               .src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA,
                               .dst_factor_rgb = SG_BLENDFACTOR_ONE,
                               .src_factor_alpha = SG_BLENDFACTOR_SRC_ALPHA,
                               .dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
                           },
                           {
                               .enabled = false,
                           });

        for (std::size_t i = 1; i <= maxClippingPaths; i++) {
            m_pathScreenPipeline[i] =
                vectorPipeline(uberShader,
                               {
                                   .enabled = true,
                                   .src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA,
                                   .dst_factor_rgb = SG_BLENDFACTOR_ONE,
                                   .src_factor_alpha = SG_BLENDFACTOR_SRC_ALPHA,
                                   .dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
                               },
                               {
                                   .enabled = true,
                                   .ref = (uint8_t)i,
                                   .read_mask = 0xFF,
                                   .write_mask = 0x00,
                                   .front =
                                       {
                                           .compare = SG_COMPAREFUNC_EQUAL,
                                       },
                                   .back =
                                       {
                                           .compare = SG_COMPAREFUNC_EQUAL,
                                       },
                               });
        }
    }

    // Additive Pipelines
    {
        m_pathAdditivePipeline[0] = vectorPipeline(uberShader,
                                                   {
                                                       .enabled = true,
                                                       .src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA,
                                                       .dst_factor_rgb = SG_BLENDFACTOR_ONE,
                                                   },
                                                   {
                                                       .enabled = false,
                                                   });

        for (std::size_t i = 1; i <= maxClippingPaths; i++) {
            m_pathAdditivePipeline[i] =
                vectorPipeline(uberShader,
                               {
                                   .enabled = true,
                                   .src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA,
                                   .dst_factor_rgb = SG_BLENDFACTOR_ONE,
                               },
                               {
                                   .enabled = true,
                                   .ref = (uint8_t)i,
                                   .read_mask = 0xFF,
                                   .write_mask = 0x00,
                                   .front =
                                       {
                                           .compare = SG_COMPAREFUNC_EQUAL,
                                       },
                                   .back =
                                       {
                                           .compare = SG_COMPAREFUNC_EQUAL,
                                       },
                               });
        }
    }

    // Multiply Pipelines
    {
        m_pathMultiplyPipeline[0] =
            vectorPipeline(uberShader,
                           {
                               .enabled = true,
                               .src_factor_rgb = SG_BLENDFACTOR_DST_COLOR,
                               .dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
                           },
                           {
                               .enabled = false,
                           });

        for (std::size_t i = 1; i <= maxClippingPaths; i++) {
            m_pathMultiplyPipeline[i] =
                vectorPipeline(uberShader,
                               {
                                   .enabled = true,
                                   .src_factor_rgb = SG_BLENDFACTOR_DST_COLOR,
                                   .dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
                               },
                               {
                                   .enabled = true,
                                   .ref = (uint8_t)i,
                                   .read_mask = 0xFF,
                                   .write_mask = 0x00,
                                   .front =
                                       {
                                           .compare = SG_COMPAREFUNC_EQUAL,
                                       },
                                   .back =
                                       {
                                           .compare = SG_COMPAREFUNC_EQUAL,
                                       },
                               });
        }
    }

    m_incClipPipeline = vectorPipeline(uberShader,
                                       {
                                           .enabled = false,
                                       },
                                       {
                                           .enabled = true,
                                           .read_mask = 0xFF,
                                           .write_mask = 0xFF,
                                           .front =
                                               {
                                                   .compare = SG_COMPAREFUNC_ALWAYS,
                                                   .depth_fail_op = SG_STENCILOP_KEEP,
                                                   .fail_op = SG_STENCILOP_KEEP,
                                                   .pass_op = SG_STENCILOP_INCR_CLAMP,
                                               },
                                           .back =
                                               {
                                                   .compare = SG_COMPAREFUNC_ALWAYS,
                                                   .depth_fail_op = SG_STENCILOP_KEEP,
                                                   .fail_op = SG_STENCILOP_KEEP,
                                                   .pass_op = SG_STENCILOP_INCR_CLAMP,
                                               },
                                       },
                                       SG_COLORMASK_NONE);

    m_decClipPipeline = vectorPipeline(uberShader,
                                       {
                                           .enabled = false,
                                       },
                                       {
                                           .enabled = true,
                                           .read_mask = 0xFF,
                                           .write_mask = 0xFF,
                                           .front =
                                               {
                                                   .compare = SG_COMPAREFUNC_ALWAYS,
                                                   .depth_fail_op = SG_STENCILOP_KEEP,
                                                   .fail_op = SG_STENCILOP_KEEP,
                                                   .pass_op = SG_STENCILOP_DECR_CLAMP,
                                               },
                                           .back =
                                               {
                                                   .compare = SG_COMPAREFUNC_ALWAYS,
                                                   .depth_fail_op = SG_STENCILOP_KEEP,
                                                   .fail_op = SG_STENCILOP_KEEP,
                                                   .pass_op = SG_STENCILOP_DECR_CLAMP,
                                               },
                                       },
                                       SG_COLORMASK_NONE);

    uint16_t indices[] = {0, 1, 2, 0, 2, 3};

    m_boundsIndices = sg_make_buffer((sg_buffer_desc){
        .type = SG_BUFFERTYPE_INDEXBUFFER,
        .data = SG_RANGE(indices),
    });
}

SokolTessRenderer::~SokolTessRenderer() {
    sg_destroy_buffer(m_boundsIndices);
    sg_destroy_pipeline(m_meshPipeline);
    sg_destroy_pipeline(m_incClipPipeline);
    sg_destroy_pipeline(m_decClipPipeline);
    for (std::size_t i = 0; i <= maxClippingPaths; i++) {
        sg_destroy_pipeline(m_pathPipeline[i]);
        sg_destroy_pipeline(m_pathScreenPipeline[i]);
    }
}

void SokolTessRenderer::orthographicProjection(float left,
                                               float right,
                                               float bottom,
                                               float top,
                                               float near,
                                               float far) {
    m_Projection[0] = 2.0f / (right - left);
    m_Projection[1] = 0.0f;
    m_Projection[2] = 0.0f;
    m_Projection[3] = 0.0f;

    m_Projection[4] = 0.0f;
    m_Projection[5] = 2.0f / (top - bottom);
    m_Projection[6] = 0.0f;
    m_Projection[7] = 0.0f;

#ifdef SOKOL_GLCORE33
    m_Projection[8] = 0.0f;
    m_Projection[9] = 0.0f;
    m_Projection[10] = 2.0f / (near - far);
    m_Projection[11] = 0.0f;

    m_Projection[12] = (right + left) / (left - right);
    m_Projection[13] = (top + bottom) / (bottom - top);
    m_Projection[14] = (far + near) / (near - far);
    m_Projection[15] = 1.0f;
#else
    // NDC are slightly different in Metal:
    // https://metashapes.com/blog/opengl-metal-projection-matrix-problem/
    m_Projection[8] = 0.0f;
    m_Projection[9] = 0.0f;
    m_Projection[10] = 1.0f / (far - near);
    m_Projection[11] = 0.0f;

    m_Projection[12] = (right + left) / (left - right);
    m_Projection[13] = (top + bottom) / (bottom - top);
    m_Projection[14] = near / (near - far);
    m_Projection[15] = 1.0f;
#endif
    // for (int i = 0; i < 4; i++) {
    //     int b = i * 4;
    //     printf("%f\t%f\t%f\t%f\n",
    //            m_Projection[b],
    //            m_Projection[b + 1],
    //            m_Projection[b + 2],
    //            m_Projection[b + 3]);
    // }
}

void SokolTessRenderer::drawImage(const RenderImage*, BlendMode, float opacity) {}
void SokolTessRenderer::drawImageMesh(const RenderImage* renderImage,
                                      rcp<RenderBuffer> vertices_f32,
                                      rcp<RenderBuffer> uvCoords_f32,
                                      rcp<RenderBuffer> indices_u16,
                                      BlendMode blendMode,
                                      float opacity) {
    vs_params_t vs_params;

    const Mat2D& world = transform();
    vs_params.mvp = m_Projection * world;

    setPipeline(m_meshPipeline);
    sg_bindings bind = {
        .vertex_buffers[0] = static_cast<SokolBuffer*>(vertices_f32.get())->buffer(),
        .vertex_buffers[1] = static_cast<SokolBuffer*>(uvCoords_f32.get())->buffer(),
        .index_buffer = static_cast<SokolBuffer*>(indices_u16.get())->buffer(),
        .fs_images[SLOT_tex] = static_cast<const SokolRenderImage*>(renderImage)->image(),
    };

    sg_apply_bindings(&bind);
    sg_apply_uniforms(SG_SHADERSTAGE_VS, SLOT_vs_params, SG_RANGE_REF(vs_params));
    sg_draw(0, indices_u16->count(), 1);
}

class SokolGradient : public RenderShader {
private:
    Vec2D m_start;
    Vec2D m_end;
    int m_type;
    std::vector<float> m_colors;
    std::vector<float> m_stops;
    bool m_isVisible = false;

private:
    // General gradient
    SokolGradient(int type, const ColorInt colors[], const float stops[], size_t count) :
        m_type(type) {
        m_stops.resize(count);
        m_colors.resize(count * 4);
        for (int i = 0, colorIndex = 0; i < count; i++, colorIndex += 4) {
            fillColorBuffer(&m_colors[colorIndex], colors[i]);
            m_stops[i] = stops[i];
            if (m_colors[colorIndex + 3] > 0.0f) {
                m_isVisible = true;
            }
        }
    }

public:
    // Linear gradient
    SokolGradient(float sx,
                  float sy,
                  float ex,
                  float ey,
                  const ColorInt colors[],
                  const float stops[],
                  size_t count) :
        SokolGradient(1, colors, stops, count) {
        m_start = Vec2D(sx, sy);
        m_end = Vec2D(ex, ey);
    }

    SokolGradient(float cx,
                  float cy,
                  float radius,
                  const ColorInt colors[], // [count]
                  const float stops[],     // [count]
                  size_t count) :
        SokolGradient(2, colors, stops, count) {
        m_start = Vec2D(cx, cy);
        m_end = Vec2D(cx + radius, cy);
    }

    void bind(fs_path_uniforms_t& uniforms) {
        auto stopCount = m_stops.size();
        uniforms.fillType = m_type;
        uniforms.stopCount = stopCount;
        uniforms.gradientStart = m_start;
        uniforms.gradientEnd = m_end;
        for (int i = 0; i < stopCount; i++) {
            auto colorBufferIndex = i * 4;
            for (int j = 0; j < 4; j++) {
                uniforms.colors[i][j] = m_colors[colorBufferIndex + j];
            }
            uniforms.stops[i / 4][i % 4] = m_stops[i];
        }
    }
};

rcp<RenderShader> SokolFactory::makeLinearGradient(float sx,
                                                   float sy,
                                                   float ex,
                                                   float ey,
                                                   const ColorInt colors[],
                                                   const float stops[],
                                                   size_t count) {
    return rcp<RenderShader>(new SokolGradient(sx, sy, ex, ey, colors, stops, count));
}

rcp<RenderShader> SokolFactory::makeRadialGradient(float cx,
                                                   float cy,
                                                   float radius,
                                                   const ColorInt colors[], // [count]
                                                   const float stops[],     // [count]
                                                   size_t count) {
    return rcp<RenderShader>(new SokolGradient(cx, cy, radius, colors, stops, count));
}

class SokolRenderPaint : public RenderPaint {
private:
    fs_path_uniforms_t m_uniforms = {0};
    rcp<RenderShader> m_shader;
    RenderPaintStyle m_style;
    std::unique_ptr<ContourStroke> m_stroke;
    bool m_strokeDirty = false;
    float m_strokeThickness = 0.0f;
    StrokeJoin m_strokeJoin;
    StrokeCap m_strokeCap;

    sg_buffer m_strokeVertexBuffer = {0};
    sg_buffer m_strokeIndexBuffer = {0};
    std::vector<std::size_t> m_StrokeOffsets;

    BlendMode m_blendMode = BlendMode::srcOver;

public:
    void color(ColorInt value) override {
        fillColorBuffer(m_uniforms.colors[0], value);
        m_uniforms.fillType = 0;
    }

    void style(RenderPaintStyle value) override {
        m_style = value;

        switch (m_style) {
            case RenderPaintStyle::fill:
                m_stroke = nullptr;
                m_strokeDirty = false;
                break;
            case RenderPaintStyle::stroke:
                m_stroke = std::make_unique<ContourStroke>();
                m_strokeDirty = true;
                break;
        }
    }

    RenderPaintStyle style() const { return m_style; }

    void thickness(float value) override {
        m_strokeThickness = value;
        m_strokeDirty = true;
    }

    void join(StrokeJoin value) override {
        m_strokeJoin = value;
        m_strokeDirty = true;
    }

    void cap(StrokeCap value) override {
        m_strokeCap = value;
        m_strokeDirty = true;
    }

    void invalidateStroke() override {
        if (m_stroke) {
            m_strokeDirty = true;
        }
    }

    void blendMode(BlendMode value) override { m_blendMode = value; }
    BlendMode blendMode() const { return m_blendMode; }

    void shader(rcp<RenderShader> shader) override { m_shader = shader; }

    void draw(SokolRenderPath* path) {
        if (m_shader) {
            static_cast<SokolGradient*>(m_shader.get())->bind(m_uniforms);
        }
        sg_apply_uniforms(SG_SHADERSTAGE_FS, SLOT_fs_path_uniforms, SG_RANGE_REF(m_uniforms));
        if (m_stroke != nullptr) {
            if (m_strokeDirty) {
                static Mat2D identity;
                m_stroke->reset();
                path->extrudeStroke(m_stroke.get(),
                                    m_strokeJoin,
                                    m_strokeCap,
                                    m_strokeThickness / 2.0f,
                                    identity);
                m_strokeDirty = false;

                const std::vector<Vec2D>& strip = m_stroke->triangleStrip();

                sg_destroy_buffer(m_strokeVertexBuffer);
                sg_destroy_buffer(m_strokeIndexBuffer);
                auto size = strip.size();
                if (size <= 2) {
                    m_strokeVertexBuffer = {0};
                    m_strokeIndexBuffer = {0};
                    return;
                }

                m_strokeVertexBuffer = sg_make_buffer((sg_buffer_desc){
                    .type = SG_BUFFERTYPE_VERTEXBUFFER,
                    .data =
                        {
                            strip.data(),
                            strip.size() * sizeof(Vec2D),
                        },
                });

                // Let's use a tris index buffer so we can keep the same sokol pipeline.
                std::vector<uint16_t> indices;

                // Build them by stroke offsets (where each offset represents a sub-path, or a move
                // to)
                m_stroke->resetRenderOffset();
                m_StrokeOffsets.clear();
                while (true) {
                    std::size_t strokeStart, strokeEnd;
                    if (!m_stroke->nextRenderOffset(strokeStart, strokeEnd)) {
                        break;
                    }
                    std::size_t length = strokeEnd - strokeStart;
                    if (length > 2) {
                        for (std::size_t i = 0, end = length - 2; i < end; i++) {
                            if ((i % 2) == 1) {
                                indices.push_back(i + strokeStart);
                                indices.push_back(i + 1 + strokeStart);
                                indices.push_back(i + 2 + strokeStart);
                            } else {
                                indices.push_back(i + strokeStart);
                                indices.push_back(i + 2 + strokeStart);
                                indices.push_back(i + 1 + strokeStart);
                            }
                        }
                        m_StrokeOffsets.push_back(indices.size());
                    }
                }

                m_strokeIndexBuffer = sg_make_buffer((sg_buffer_desc){
                    .type = SG_BUFFERTYPE_INDEXBUFFER,
                    .data =
                        {
                            indices.data(),
                            indices.size() * sizeof(uint16_t),
                        },
                });
            }
            if (m_strokeVertexBuffer.id == 0) {
                return;
            }

            sg_bindings bind = {
                .vertex_buffers[0] = m_strokeVertexBuffer,
                .index_buffer = m_strokeIndexBuffer,
            };

            sg_apply_bindings(&bind);

            m_stroke->resetRenderOffset();
            // path->drawStroke(m_stroke.get());
            std::size_t start = 0;
            for (auto end : m_StrokeOffsets) {
                sg_draw(start, end - start, 1);
                start = end;
            }

        } else {
            path->drawFill();
        }
    }
};

std::unique_ptr<RenderPaint> SokolFactory::makeRenderPaint() {
    return std::make_unique<SokolRenderPaint>();
}

void SokolTessRenderer::restore() {
    TessRenderer::restore();
    if (m_Stack.size() == 1) {
        // When we've fully restored, immediately update clip to not wait for next draw.
        applyClipping();
        m_currentPipeline = {0};
    }
}

void SokolTessRenderer::applyClipping() {
    if (!m_IsClippingDirty) {
        return;
    }
    m_IsClippingDirty = false;
    RenderState& state = m_Stack.back();

    auto currentClipLength = m_ClipPaths.size();
    if (currentClipLength == state.clipPaths.size()) {
        // Same length so now check if they're all the same.
        bool allSame = true;
        for (std::size_t i = 0; i < currentClipLength; i++) {
            if (state.clipPaths[i].path() != m_ClipPaths[i].path()) {
                allSame = false;
                break;
            }
        }
        if (allSame) {
            return;
        }
    }

    vs_path_params_t vs_params;
    fs_path_uniforms_t uniforms = {0};

    // Decr any paths from the last clip that are gone.
    std::unordered_set<RenderPath*> alreadyApplied;

    for (auto appliedPath : m_ClipPaths) {
        bool decr = true;
        for (auto nextClipPath : state.clipPaths) {
            if (nextClipPath.path() == appliedPath.path()) {
                decr = false;
                alreadyApplied.insert(appliedPath.path());
                break;
            }
        }
        if (decr) {
            // Draw appliedPath.path() with decr pipeline
            setPipeline(m_decClipPipeline);
            vs_params.mvp = m_Projection * appliedPath.transform();
            sg_apply_uniforms(SG_SHADERSTAGE_VS, SLOT_vs_path_params, SG_RANGE_REF(vs_params));
            sg_apply_uniforms(SG_SHADERSTAGE_FS, SLOT_fs_path_uniforms, SG_RANGE_REF(uniforms));
            auto sokolPath = static_cast<SokolRenderPath*>(appliedPath.path());
            sokolPath->drawFill();
        }
    }

    // Incr any paths that are added.
    for (auto nextClipPath : state.clipPaths) {
        if (alreadyApplied.count(nextClipPath.path())) {
            // Already applied.
            continue;
        }
        // Draw nextClipPath.path() with incr pipeline
        setPipeline(m_incClipPipeline);
        vs_params.mvp = m_Projection * nextClipPath.transform();
        sg_apply_uniforms(SG_SHADERSTAGE_VS, SLOT_vs_path_params, SG_RANGE_REF(vs_params));
        sg_apply_uniforms(SG_SHADERSTAGE_FS, SLOT_fs_path_uniforms, SG_RANGE_REF(uniforms));
        auto sokolPath = static_cast<SokolRenderPath*>(nextClipPath.path());
        sokolPath->drawFill();
    }

    // Pick which pipeline to use for draw path operations.
    // TODO: something similar for draw mesh.
    m_clipCount = state.clipPaths.size();

    m_ClipPaths = state.clipPaths;
}

void SokolTessRenderer::reset() { m_currentPipeline = {0}; }
void SokolTessRenderer::setPipeline(sg_pipeline pipeline) {
    if (m_currentPipeline.id == pipeline.id) {
        return;
    }
    m_currentPipeline = pipeline;
    sg_apply_pipeline(pipeline);
}

void SokolTessRenderer::drawPath(RenderPath* path, RenderPaint* paint) {
    auto sokolPaint = static_cast<SokolRenderPaint*>(paint);

    applyClipping();
    vs_path_params_t vs_params;
    const Mat2D& world = transform();

    vs_params.mvp = m_Projection * world;
    switch (sokolPaint->blendMode()) {
        case BlendMode::srcOver: setPipeline(m_pathPipeline[m_clipCount]); break;
        case BlendMode::screen: setPipeline(m_pathScreenPipeline[m_clipCount]); break;
        case BlendMode::colorDodge: setPipeline(m_pathAdditivePipeline[m_clipCount]); break;
        case BlendMode::multiply: setPipeline(m_pathMultiplyPipeline[m_clipCount]); break;
        default: setPipeline(m_pathScreenPipeline[m_clipCount]); break;
    }
    sg_apply_uniforms(SG_SHADERSTAGE_VS, SLOT_vs_params, SG_RANGE_REF(vs_params));

    static_cast<SokolRenderPaint*>(paint)->draw(static_cast<SokolRenderPath*>(path));
}
