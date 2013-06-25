#include "libretro.h"
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <algorithm>
#include <string>
#include "rpng/rpng.h"

#include "glm/glm.hpp"
#include "glm/gtc/matrix_transform.hpp"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
static struct retro_hw_render_callback hw_render;

using namespace glm;

#define GL_GLEXT_PROTOTYPES
#if defined(GLES)
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#else
#include <GL/glew.h>
#include <GL/gl.h>
#endif

static std::string texpath;

static GLuint prog;
static GLuint vbo, mbo;
static GLuint ibo;
static GLuint tex;
static bool update;

static vec3 player_pos;

static float camera_rot_x;
static float camera_rot_y;

struct Vertex
{
   GLfloat vert[4];
   GLfloat normal[4];
   GLfloat tex[2];
};

static const Vertex vertex_data[] = {
   { { -1, -1, -1, 1 }, { 0, 0, -1, 0 }, { 0, 0 } }, // Front
   { {  1, -1, -1, 1 }, { 0, 0, -1, 0 }, { 1, 0 } },
   { { -1,  1, -1, 1 }, { 0, 0, -1, 0 }, { 0, 1 } },
   { {  1,  1, -1, 1 }, { 0, 0, -1, 0 }, { 1, 1 } },

   { {  1, -1,  1, 1 }, { 0, 0,  1, 0 }, { 0, 0 } }, // Back
   { { -1, -1,  1, 1 }, { 0, 0,  1, 0 }, { 1, 0 } },
   { {  1,  1,  1, 1 }, { 0, 0,  1, 0 }, { 0, 1 } },
   { { -1,  1,  1, 1 }, { 0, 0,  1, 0 }, { 1, 1 } },
   
   { { -1, -1,  1, 1 }, { -1, 0, 0, 0 }, { 0, 0 } }, // Left
   { { -1, -1, -1, 1 }, { -1, 0, 0, 0 }, { 1, 0 } },
   { { -1,  1,  1, 1 }, { -1, 0, 0, 0 }, { 0, 1 } },
   { { -1,  1, -1, 1 }, { -1, 0, 0, 0 }, { 1, 1 } },

   { { 1, -1, -1, 1 }, { 1, 0, 0, 0 }, { 0, 0 } }, // Right
   { { 1, -1,  1, 1 }, { 1, 0, 0, 0 }, { 1, 0 } },
   { { 1,  1, -1, 1 }, { 1, 0, 0, 0 }, { 0, 1 } },
   { { 1,  1,  1, 1 }, { 1, 0, 0, 0 }, { 1, 1 } },

   { { -1,  1, -1, 1 }, { 0, 1, 0, 0 }, { 0, 0 } }, // Top
   { {  1,  1, -1, 1 }, { 0, 1, 0, 0 }, { 1, 0 } },
   { { -1,  1,  1, 1 }, { 0, 1, 0, 0 }, { 0, 1 } },
   { {  1,  1,  1, 1 }, { 0, 1, 0, 0 }, { 1, 1 } },

   { { -1, -1,  1, 1 }, { 0, -1, 0, 0 }, { 0, 0 } }, // Bottom
   { {  1, -1,  1, 1 }, { 0, -1, 0, 0 }, { 1, 0 } },
   { { -1, -1, -1, 1 }, { 0, -1, 0, 0 }, { 0, 1 } },
   { {  1, -1, -1, 1 }, { 0, -1, 0, 0 }, { 1, 1 } },
};

static const GLubyte indices[] = {
   0, 1, 2, // Front
   3, 2, 1,

   4, 5, 6, // Back
   7, 6, 5,

   8, 9, 10, // Left
   11, 10, 9,

   12, 13, 14, // Right
   15, 14, 13,

   16, 17, 18, // Top
   19, 18, 17,

   20, 21, 22, // Bottom
   23, 22, 21,
};

static const char *vertex_shader[] = {
   "uniform mat4 uVP;",
   "uniform mat4 uM;",
   "attribute vec4 aVertex;",
   "attribute vec4 aNormal;",
   "attribute vec2 aTexCoord;",
   "attribute vec4 aOffset;",
   "varying vec3 normal;",
   "varying vec4 model_pos;",
   "varying vec2 tex_coord;",
   "void main() {",
   "  model_pos = uM * (aVertex + aOffset);",
   "  gl_Position = uVP * model_pos;",
   "  vec4 trans_normal = uM * aNormal;",
   "  normal = trans_normal.xyz;",
   "  tex_coord = aTexCoord;",
   "}",
};

static const char *fragment_shader[] = {
   "varying vec3 normal;",
   "varying vec4 model_pos;",
   "varying vec2 tex_coord;",
   "uniform vec3 light_pos;",
   "uniform vec4 ambient_light;",
   "uniform sampler2D uTexture;",

   "void main() {",
   "  vec3 diff = light_pos - model_pos.xyz;",
   "  float dist_mod = 100.0 * inversesqrt(dot(diff, diff));",
   "  gl_FragColor = texture2D(uTexture, tex_coord) * (ambient_light + dist_mod * smoothstep(0.0, 1.0, dot(normalize(diff), normal)));",
   "}",
};

static void print_shader_log(GLuint shader)
{
   GLsizei len = 0;
   glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &len);
   if (!len)
      return;

   char *buffer = new char[len];
   glGetShaderInfoLog(shader, len, &len, buffer);
   fprintf(stderr, "Info Log: %s\n", buffer);
   delete[] buffer;
}

static void compile_program(void)
{
   prog = glCreateProgram();
   GLuint vert = glCreateShader(GL_VERTEX_SHADER);
   GLuint frag = glCreateShader(GL_FRAGMENT_SHADER);

   glShaderSource(vert, ARRAY_SIZE(vertex_shader), vertex_shader, 0);
   glShaderSource(frag, ARRAY_SIZE(fragment_shader), fragment_shader, 0);
   glCompileShader(vert);
   glCompileShader(frag);

   int status = 0;
   glGetShaderiv(vert, GL_COMPILE_STATUS, &status);
   if (!status)
   {
      fprintf(stderr, "Vertex shader failed to compile!\n");
      print_shader_log(vert);
   }
   glGetShaderiv(frag, GL_COMPILE_STATUS, &status);
   if (!status)
   {
      fprintf(stderr, "Fragment shader failed to compile!\n");
      print_shader_log(frag);
   }

   glAttachShader(prog, vert);
   glAttachShader(prog, frag);
   glLinkProgram(prog);

   glGetProgramiv(prog, GL_LINK_STATUS, &status);
   if (!status)
      fprintf(stderr, "Program failed to link!\n");
}

static unsigned cube_size = 1;

static void setup_vao(void)
{
   glUseProgram(prog);

   glGenBuffers(1, &vbo);
   glBindBuffer(GL_ARRAY_BUFFER, vbo);
   glBufferData(GL_ARRAY_BUFFER, sizeof(vertex_data), vertex_data, GL_STATIC_DRAW);

   glGenBuffers(1, &mbo);
   glBindBuffer(GL_ARRAY_BUFFER, mbo);
   glBufferData(GL_ARRAY_BUFFER, cube_size * cube_size * cube_size * sizeof(GLfloat) * 4, NULL, GL_STREAM_DRAW);

   glGenBuffers(1, &ibo);
   glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
   glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

   glBindBuffer(GL_ARRAY_BUFFER, 0);
   glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
   glUseProgram(0);

   update = true;
}

static GLuint load_texture(const char *path)
{
   uint32_t *data;
   unsigned width, height;
   if (!rpng_load_image_argb(path, &data, &width, &height))
   {
      fprintf(stderr, "Couldn't load texture: %s\n", path);
      return 0;
   }

   GLuint tex;
   glGenTextures(1, &tex);
   glBindTexture(GL_TEXTURE_2D, tex);

#ifdef GLES
   glTexImage2D(GL_TEXTURE_2D, 0, GL_BGRA_EXT, width, height,
         0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, data);
#else
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height,
         0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, data);
#endif
   free(data);

   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   return tex;
}

void retro_init(void)
{}

void retro_deinit(void)
{}

unsigned retro_api_version(void)
{
   return RETRO_API_VERSION;
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
   (void)port;
   (void)device;
}

void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof(*info));
   info->library_name     = "InstancingViewer GL";
   info->library_version  = "v1";
   info->need_fullpath    = false;
   info->valid_extensions = "png";
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   info->timing.fps = 60.0;
   info->timing.sample_rate = 30000.0;

   info->geometry.base_width  = 640;
   info->geometry.base_height = 480;
   info->geometry.max_width   = 640;
   info->geometry.max_height  = 480;
}

static retro_video_refresh_t video_cb;
static retro_audio_sample_t audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_environment_t environ_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;

void retro_set_environment(retro_environment_t cb)
{
   environ_cb = cb;

   struct retro_variable variables[] = {
      { "cube_size",
         "Cube size; 1|2|4|8|16|32|64|128" },
      { NULL, NULL },
   };

   environ_cb(RETRO_ENVIRONMENT_SET_VARIABLES, variables);
}

void retro_set_audio_sample(retro_audio_sample_t cb)
{
   audio_cb = cb;
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
   audio_batch_cb = cb;
}

void retro_set_input_poll(retro_input_poll_t cb)
{
   input_poll_cb = cb;
}

void retro_set_input_state(retro_input_state_t cb)
{
   input_state_cb = cb;
}

void retro_set_video_refresh(retro_video_refresh_t cb)
{
   video_cb = cb;
}

static vec3 check_input()
{
   input_poll_cb();

   int x = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_X);
   int y = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_Y);
   x = std::max(std::min(x, 20), -20);
   y = std::max(std::min(y, 20), -20);
   camera_rot_x -= 0.20 * x;
   camera_rot_y -= 0.10 * y;

   camera_rot_y = std::max(std::min(camera_rot_y, 80.0f), -80.0f);

   mat4 look_rot_x = rotate(mat4(1.0), camera_rot_x, vec3(0, 1, 0));
   mat4 look_rot_y = rotate(mat4(1.0), camera_rot_y, vec3(1, 0, 0));
   vec3 look_dir = vec3(look_rot_x * look_rot_y * vec4(0, 0, -1, 0));

   vec3 look_dir_side = vec3(look_rot_x * vec4(1, 0, 0, 0));

   mat3 s = mat3(scale(mat4(1.0), vec3(0.25, 0.25, 0.25)));
   if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP))
      player_pos += s * look_dir;
   if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN))
      player_pos -= s * look_dir;

   if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT))
      player_pos -= s * look_dir_side;
   if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT))
      player_pos += s * look_dir_side;

   return look_dir;
}

static void context_reset(void)
{
   fprintf(stderr, "Context reset!\n");
#ifndef GLES
   glewExperimental = GL_TRUE;
   glewInit();
#endif
   compile_program();
   setup_vao();
   tex = load_texture(texpath.c_str());
}

static bool first_init = true;

static void update_variables(void)
{
   struct retro_variable var;
   
   var.key = "cube_size";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
   {
      cube_size = atoi(var.value);
      update = true;

      if (!first_init)
         context_reset();
   }
}

void retro_run(void)
{
   bool updated = false;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
      update_variables();

   vec3 look_dir = check_input();

   glBindFramebuffer(GL_FRAMEBUFFER, hw_render.get_current_framebuffer());
   glClearColor(0.1, 0.1, 0.1, 1.0);
   glViewport(0, 0, 640, 480);
   glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

   glUseProgram(prog);

   glBindBuffer(GL_ARRAY_BUFFER, vbo);
   int vloc = glGetAttribLocation(prog, "aVertex");
   glVertexAttribPointer(vloc, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)(offsetof(Vertex, vert)));
   glEnableVertexAttribArray(vloc);
   int nloc = glGetAttribLocation(prog, "aNormal");
   glVertexAttribPointer(nloc, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)(offsetof(Vertex, normal)));
   glEnableVertexAttribArray(nloc);
   int tcloc = glGetAttribLocation(prog, "aTexCoord");
   glVertexAttribPointer(tcloc, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)(offsetof(Vertex, tex)));
   glEnableVertexAttribArray(tcloc);

   glBindBuffer(GL_ARRAY_BUFFER, mbo);
   int mloc = glGetAttribLocation(prog, "aOffset");
   glVertexAttribPointer(mloc, 4, GL_FLOAT, GL_FALSE, 0, 0);
   glVertexAttribDivisor(mloc, 1); // Update per instance.
   glEnableVertexAttribArray(mloc);


   glEnable(GL_DEPTH_TEST);
   glEnable(GL_CULL_FACE);

   int tloc = glGetUniformLocation(prog, "uTexture");
   glUniform1i(tloc, 0);
   glActiveTexture(GL_TEXTURE0);
   glBindTexture(GL_TEXTURE_2D, tex);

   int lloc = glGetUniformLocation(prog, "light_pos");
   vec3 light_pos(0, 150, 15);
   glUniform3fv(lloc, 1, &light_pos[0]);

   vec4 ambient_light(0.2, 0.2, 0.2, 1.0);
   lloc = glGetUniformLocation(prog, "ambient_light");
   glUniform4fv(lloc, 1, &ambient_light[0]);

   int vploc = glGetUniformLocation(prog, "uVP");
   mat4 view = lookAt(player_pos, player_pos + look_dir, vec3(0, 1, 0));
   mat4 proj = scale(mat4(1.0), vec3(1, -1, 1)) * perspective(45.0f, 640.0f / 480.0f, 5.0f, 500.0f);
   mat4 vp = proj * view;
   glUniformMatrix4fv(vploc, 1, GL_FALSE, &vp[0][0]);

   int modelloc = glGetUniformLocation(prog, "uM");
   mat4 model = mat4(1.0);
   glUniformMatrix4fv(modelloc, 1, GL_FALSE, &model[0][0]);

   if (update)
   {
      update = false;
      glBindBuffer(GL_ARRAY_BUFFER, mbo);

      GLfloat *buf = (GLfloat*)glMapBufferRange(GL_ARRAY_BUFFER, 0,
            cube_size * cube_size * cube_size * 4 * sizeof(GLfloat),
            GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);

      for (unsigned x = 0; x < cube_size; x++)
         for (unsigned y = 0; y < cube_size; y++)
            for (unsigned z = 0; z < cube_size; z++)
            {
               GLfloat *off = buf + 4 * ((cube_size * cube_size * z) + (cube_size * y) + x);
               off[0] = 4.0f * ((float)x - cube_size / 2);
               off[1] = 4.0f * ((float)y - cube_size / 2);
               off[2] = -100.0f + 4.0f * ((float)z - cube_size / 2);
            }
      glUnmapBuffer(GL_ARRAY_BUFFER);
      glBindBuffer(GL_ARRAY_BUFFER, 0);
   }
   
   glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
   glDrawElementsInstanced(GL_TRIANGLES, ARRAY_SIZE(indices),
         GL_UNSIGNED_BYTE, NULL, cube_size * cube_size * cube_size);

   glUseProgram(0);
   glBindBuffer(GL_ARRAY_BUFFER, 0);
   glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
   glDisableVertexAttribArray(vloc);
   glDisableVertexAttribArray(nloc);
   glDisableVertexAttribArray(tcloc);
   glDisableVertexAttribArray(mloc);
   glBindTexture(GL_TEXTURE_2D, 0);

   video_cb(RETRO_HW_FRAME_BUFFER_VALID, 640, 480, 0);
}


bool retro_load_game(const struct retro_game_info *info)
{
   update_variables();

   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
   {
      fprintf(stderr, "XRGB8888 is not supported.\n");
      return false;
   }

#ifdef GLES
   hw_render.context_type = RETRO_HW_CONTEXT_OPENGLES2;
#else
   hw_render.context_type = RETRO_HW_CONTEXT_OPENGL;
#endif
   hw_render.context_reset = context_reset;
   hw_render.depth = true;
   if (!environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render))
      return false;

   fprintf(stderr, "Loaded game!\n");
   player_pos = vec3(0, 0, 0);
   texpath = info->path;

   first_init = false;

   return true;
}

void retro_unload_game(void)
{}

unsigned retro_get_region(void)
{
   return RETRO_REGION_NTSC;
}

bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num)
{
   (void)type;
   (void)info;
   (void)num;
   return false;
}

size_t retro_serialize_size(void)
{
   return 0;
}

bool retro_serialize(void *data, size_t size)
{
   (void)data;
   (void)size;
   return false;
}

bool retro_unserialize(const void *data, size_t size)
{
   (void)data;
   (void)size;
   return false;
}

void *retro_get_memory_data(unsigned id)
{
   (void)id;
   return NULL;
}

size_t retro_get_memory_size(unsigned id)
{
   (void)id;
   return 0;
}

void retro_reset(void)
{}

void retro_cheat_reset(void)
{}

void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
   (void)index;
   (void)enabled;
   (void)code;
}

