#include <hal/video.h>
#include <hal/debug.h>
#include <SDL_image.h>
#include <nxdk/mount.h>
#include <windows.h>

#include "../../common/xgu/xgu.h"
#include "../../common/xgu/xgux.h"
#include "../../common/input.h"
#include "../../common/math.h"
#include "../../common/swizzle.h"

typedef struct FormatInfo {
    SDL_PixelFormatEnum SdlFormat;
    XguTexFormatColor XguFormat;
    bool XguSwizzled;
    bool RequireConversion;
    char* Name;
} FormatInfo;

typedef struct Vertex {
    float pos[3];
    float texcoord[2];
    float normal[3];
} Vertex;

#include "verts.h"

static void init_shader(void) {
    XguTransformProgramInstruction vs_program[] = {
        #include "vshader.inl"
    };
    
    uint32_t *p = pb_begin();
    
    p = xgu_set_transform_program_start(p, 0);
    
    p = xgu_set_transform_execution_mode(p, XGU_PROGRAM, XGU_RANGE_MODE_PRIVATE);
    p = xgu_set_transform_program_cxt_write_enable(p, false);
    
    p = xgu_set_transform_program_load(p, 0);
    
    // FIXME: wait for xgu_set_transform_program to get fixed
    for(int i = 0; i < sizeof(vs_program)/16; i++) {
        p = push_command(p, NV097_SET_TRANSFORM_PROGRAM, 4);
        p = push_parameters(p, &vs_program[i].i[0], 4);
    }
    
    pb_end(p);
    
    p = pb_begin();
    #include "combiner.inl"
    pb_end(p);
}

// bitscan forward
int bsf(int val) {
    __asm bsf eax, val
}

// checks if the value is a power of 2
bool is_pow2(int val) {
    return (val & (val - 1)) == 0;
}

int main(void) {

    int width = 640, height = 480;
    int format_map_index = 0;
    bool toggleFormat;

    const FormatInfo format_map[] = {

        // swizzled
        { SDL_PIXELFORMAT_ABGR8888, XGU_TEXTURE_FORMAT_A8B8G8R8_SWIZZLED, true, false, "SZ_A8B8G8R8" },
        { SDL_PIXELFORMAT_RGBA8888, XGU_TEXTURE_FORMAT_R8G8B8A8_SWIZZLED, true, false, "SZ_R8G8B8A8" },
        { SDL_PIXELFORMAT_ARGB1555, XGU_TEXTURE_FORMAT_A1R5G5B5_SWIZZLED, true, false, "SZ_A1R5G5B5" },
        { SDL_PIXELFORMAT_ARGB1555, XGU_TEXTURE_FORMAT_X1R5G5B5_SWIZZLED, true, false, "SZ_X1R5G5B5" },
        { SDL_PIXELFORMAT_ARGB4444, XGU_TEXTURE_FORMAT_A4R4G4B4_SWIZZLED, true, false, "SZ_A4R4G4B4" },
        { SDL_PIXELFORMAT_RGB565, XGU_TEXTURE_FORMAT_R5G6B5_SWIZZLED, true, false, "SZ_R5G6B5" },
        { SDL_PIXELFORMAT_ARGB8888, XGU_TEXTURE_FORMAT_A8R8G8B8_SWIZZLED, true, false, "SZ_A8R8G8B8" },
        { SDL_PIXELFORMAT_ARGB8888, XGU_TEXTURE_FORMAT_X8R8G8B8_SWIZZLED, true, false, "SZ_X8R8G8B8" },
        { SDL_PIXELFORMAT_BGRA8888, 0x3B, true, false, "SZ_B8G8R8A8" }, 

        // linear
        { SDL_PIXELFORMAT_ARGB1555, XGU_TEXTURE_FORMAT_A1R5G5B5, false, false, "A1R5G5B5" },
        { SDL_PIXELFORMAT_RGB565, XGU_TEXTURE_FORMAT_R5G6B5, false, false, "R5G6B5" },
        { SDL_PIXELFORMAT_ARGB8888, XGU_TEXTURE_FORMAT_A8R8G8B8, false, false, "A8R8G8B8" },
        { SDL_PIXELFORMAT_ARGB1555, XGU_TEXTURE_FORMAT_X1R5G5B5, false, false, "X1R5G5B5" },
        { SDL_PIXELFORMAT_ARGB4444, XGU_TEXTURE_FORMAT_A4R4G4B4, false, false, "A4R4G4B4" },
        { SDL_PIXELFORMAT_ARGB8888, XGU_TEXTURE_FORMAT_X8R8G8B8, false, false, "X8R8G8B8" },
        { SDL_PIXELFORMAT_ABGR8888, XGU_TEXTURE_FORMAT_A8B8G8R8, false, false, "A8B8G8R8" },
        { SDL_PIXELFORMAT_BGRA8888, XGU_TEXTURE_FORMAT_B8G8R8A8, false, false, "B8G8R8A8" },
        { SDL_PIXELFORMAT_RGBA8888, XGU_TEXTURE_FORMAT_R8G8B8A8, false, false, "R8G8B8A8" },

        // yuv
        //{ SDL_PIXELFORMAT_RGB888, 0x24, false, true, "UY2" },   // CR8YB8CB8YA8 aka YUY2?
        //{ SDL_PIXELFORMAT_RGB888, 0x25, false, true, "UYVY" },  // YB8CR8YA8CB8 aka UYVY?

        // misc formats - generate specific gradient color in texture buffer to discern between swizzled and non-swizzled
        //{ SDL_PIXELFORMAT_UNKNOWN, XGU_TEXTURE_FORMAT_Y8_SWIZZLED, true, true, "SZ_Y8" },
        // TODO: define others here
    };

    XguMatrix4x4 m_model, m_view, m_proj, m_viewport;
    XguVec4 v_obj_rot   = {  0,   0,   0,  1 };
    XguVec4 v_obj_scale = {  1,   1,   1,  1 };
    XguVec4 v_obj_pos   = {  0,   0,   0,  1 };
    XguVec4 v_cam_pos   = {  0,   0,   1,  1 };
    XguVec4 v_cam_rot   = {  0,   0,   0,  1 };
    XguVec4 v_light_dir = {  0,   0,   1,  1 };

    mtx_identity(&m_view);
    mtx_world_view(&m_view, v_cam_pos, v_cam_rot);
    mtx_identity(&m_proj);
    mtx_view_screen(&m_proj, (float)width/(float)height, 60.0f, 1.0f, 10000.0f);
    mtx_viewport(&m_viewport, 0, 0, width, height, 0, (float)0xFFFFFF);
    mtx_multiply(&m_proj, m_proj, m_viewport);
    mtx_identity(&m_model);

    XVideoSetMode(width, height, 32, REFRESH_DEFAULT);

    // Mount C as B to prevent issues with debug kernels
    debugPrint("Mounting C Drive as B...");
    if (!nxMountDrive('B', "\\Device\\Harddisk0\\Partition2\\")) {
        debugPrint("failed!\n");
        Sleep(3000);
        return 1;
    } else debugPrint("done!\n");
    
/*
    // Mount E
    debugPrint("Mounting E Drive...");
    if (!nxMountDrive('E', "\\Device\\Harddisk0\\Partition1\\")) {
        debugPrint("failed!\n");
        Sleep(3000);
        return 1;
    } else debugPrint("done!\n");

    // Mount F
    debugPrint("Mounting F Drive...");
    if (!nxMountDrive('F', "\\Device\\Harddisk0\\Partition6\\")) {
        debugPrint("failed!\n");
        Sleep(3000);
        return 1;
    } else debugPrint("done!\n");
*/

    // TODO: get xbe directory and use relative paths for loading resources
    // load texture
    debugPrint("Loading texture...");
    SDL_Surface *src_tex = IMG_Load("D:\\media\\texture.png");   // TODO: if loading xbe from hdd rather than an iso update this to the correct path
    if (!src_tex) {
        debugPrint("failed!\n");
        Sleep(3000);
        return 1;
    } else debugPrint("done!\n");

    // ensure texture dimensions are a power of 2
    assert(is_pow2(src_tex->w));
    assert(is_pow2(src_tex->h));

    // allocate vertices memory
    Vertex *alloc_vertices = MmAllocateContiguousMemoryEx(sizeof(vertices), 0, 0x03FFAFFF, 0, PAGE_WRITECOMBINE | PAGE_READWRITE);
    memcpy(alloc_vertices, vertices, sizeof(vertices));
    uint32_t num_vertices = sizeof(vertices)/sizeof(vertices[0]);
    
    // buffer to hold converted texture data, assumes no more than 32bpp
    uint8_t *dst_tex_buf = MmAllocateContiguousMemoryEx(src_tex->w * src_tex->h * 4, 0, 0x03FFAFFF, 0, PAGE_WRITECOMBINE | PAGE_READWRITE);
    SDL_Surface *dst_txt = SDL_ConvertSurfaceFormat(src_tex, format_map[format_map_index].SdlFormat, 0);
    if (format_map[format_map_index].XguSwizzled) {
        swizzle_rect((uint8_t*)dst_txt->pixels, dst_txt->w, dst_txt->h, dst_tex_buf, dst_txt->pitch, dst_txt->format->BytesPerPixel);
    } else {
        memcpy(dst_tex_buf, dst_txt->pixels, dst_txt->pitch * dst_txt->h);
    }

    // HACK: normalize texture coords based on swizzle status, not sure why this is required yet...
    for (int i = 0; i < num_vertices; i++) {
        float val = format_map[format_map_index].XguSwizzled ? 1.0f : 256.0f;
        if (alloc_vertices[i].texcoord[0]) alloc_vertices[i].texcoord[0] = val;
        if (alloc_vertices[i].texcoord[1]) alloc_vertices[i].texcoord[1] = val;
    } 

    input_init();
    pb_init();
    pb_show_front_screen();
    init_shader();

    while(1) {
        input_poll();

        if(input_button_down(SDL_CONTROLLER_BUTTON_START))
            break;

        // switch texture format
        if (input_button_down(SDL_CONTROLLER_BUTTON_A)) {
            if (toggleFormat) {

                // next index, wrapping if necessary
                format_map_index = (format_map_index + 1) % (sizeof(format_map) / sizeof(format_map[0]));

                // convert the source texture to the desired format
                SDL_FreeSurface(dst_txt);                
                dst_txt = SDL_ConvertSurfaceFormat(src_tex, format_map[format_map_index].SdlFormat, 0);
                if (format_map[format_map_index].XguSwizzled) {
                    swizzle_rect((uint8_t*)dst_txt->pixels, dst_txt->w, dst_txt->h, dst_tex_buf, dst_txt->pitch, dst_txt->format->BytesPerPixel);
                } else {
                    memcpy(dst_tex_buf, dst_txt->pixels, dst_txt->pitch * dst_txt->h);
                }

                // HACK: normalize texture coords based on swizzle status, not sure why this is required yet...
                for (int i = 0; i < num_vertices; i++) {
                    float val = format_map[format_map_index].XguSwizzled ? 1.0f : 256.0f;
                    if (alloc_vertices[i].texcoord[0]) alloc_vertices[i].texcoord[0] = val;
                    if (alloc_vertices[i].texcoord[1]) alloc_vertices[i].texcoord[1] = val;
                } 
            }
            toggleFormat = false;
        } else toggleFormat = true;

        pb_wait_for_vbl();
        pb_reset();
        pb_target_back_buffer();
        
        while(pb_busy());

        uint32_t *p = pb_begin();
        
        p = xgu_set_color_clear_value(p, 0xffffffff);
        p = xgu_set_zstencil_clear_value(p, 0xffffff00);
        p = xgu_clear_surface(p, XGU_CLEAR_Z | XGU_CLEAR_STENCIL | XGU_CLEAR_COLOR);
        p = xgu_set_front_face(p, XGU_FRONT_CCW);

        // Texture 0
        p = xgu_set_texture_offset(p, 0, (void *)((uint32_t)dst_tex_buf & 0x03ffffff));
        if (format_map[format_map_index].XguSwizzled) {
            p = xgu_set_texture_format(p, 0, 2, false, XGU_SOURCE_COLOR, 2, format_map[format_map_index].XguFormat, 1, bsf(dst_txt->w), bsf(dst_txt->h), 0);
            p = xgu_set_texture_address(p, 0, XGU_CLAMP_TO_EDGE, false, XGU_CLAMP_TO_EDGE, false, XGU_CLAMP_TO_EDGE, false, false);
            p = xgu_set_texture_control0(p, 0, true, 0, 0);
        } else {
            p = xgu_set_texture_format(p, 0, 2, false, XGU_SOURCE_COLOR, 2, format_map[format_map_index].XguFormat, 1, 0, 0, 0);
            p = xgu_set_texture_control0(p, 0, true, 0, 0);
            p = xgu_set_texture_control1(p, 0, dst_txt->pitch);
            p = xgu_set_texture_image_rect(p, 0, dst_txt->w, dst_txt->h);
        }

        // Pass constants to the vertex shader program
        p = xgu_set_transform_constant_load(p, 96);
        
        p = xgu_set_transform_constant(p, (XguVec4 *)&m_model, 4);
        p = xgu_set_transform_constant(p, (XguVec4 *)&m_view, 4);
        p = xgu_set_transform_constant(p, (XguVec4 *)&m_proj, 4);
        
        p = xgu_set_transform_constant(p, &v_cam_pos, 1);
        p = xgu_set_transform_constant(p, &v_light_dir, 1);
        
        XguVec4 constants = {0, 0, 0, 0};
        p = xgu_set_transform_constant(p, &constants, 1);
        
        pb_end(p);
        
        // Clear all attributes
        for(int i = 0; i < XGU_ATTRIBUTE_COUNT; i++) {
            xgux_set_attrib_pointer(i, XGU_FLOAT, 0, 0, NULL);
        }

        xgux_set_attrib_pointer(XGU_VERTEX_ARRAY, XGU_FLOAT, 3, sizeof(alloc_vertices[0]), &alloc_vertices[0].pos[0]);
        xgux_set_attrib_pointer(XGU_TEXCOORD0_ARRAY, XGU_FLOAT, 2, sizeof(alloc_vertices[0]), &alloc_vertices[0].texcoord[0]);
        xgux_set_attrib_pointer(XGU_NORMAL_ARRAY, XGU_FLOAT, 3, sizeof(alloc_vertices[0]), &alloc_vertices[0].normal[0]);
        
        xgux_draw_arrays(XGU_TRIANGLES, 0, num_vertices);

        while(pb_busy());
        while(pb_finished());
    }
    
    input_free();
    
    MmFreeContiguousMemory(alloc_vertices);
    MmFreeContiguousMemory(dst_tex_buf);
    SDL_FreeSurface(src_tex);
    SDL_FreeSurface(dst_txt);

    pb_show_debug_screen();
    pb_kill();
    return 0;
}
