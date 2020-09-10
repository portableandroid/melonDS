#include <ctime>
#include <string>
#include <vector>

#include <libretro.h>
#include <streams/file_stream.h>
#include <streams/file_stream_transforms.h>
#include <file/file_path.h>

#include "Config.h"
#include "Platform.h"
#include "NDS.h"
#include "GPU.h"
#include "SPU.h"
#include "version.h"

#include "input.h"
#include "opengl.h"
#include "screenlayout.h"
#include "utils.h"

#ifdef PORTANDROID
#define DEBUG_LEVEL 1
#include "emu_init.h"
#endif

char retro_base_directory[4096];
static char retro_saves_directory[4096];

struct retro_log_callback logging;

retro_audio_sample_batch_t audio_cb;
retro_environment_t environ_cb;
retro_input_poll_t input_poll_cb;
retro_input_state_t input_state_cb;
retro_log_printf_t log_cb;
retro_video_refresh_t video_cb;

std::string rom_path;
std::string save_path;

GPU::RenderSettings video_settings;

bool enable_opengl = false;
bool using_opengl = false;
bool refresh_opengl = true;
bool swapped_screens = false;
bool toggle_swap_screen = false;
bool swap_screen_toggled = false;

enum CurrentRenderer
{
   None,
   Software,
   OpenGL,
};

static CurrentRenderer current_renderer = CurrentRenderer::None;

bool direct_boot = false;

static void fallback_log(enum retro_log_level level, const char *fmt, ...)
{
   (void)level;
   va_list va;
   va_start(va, fmt);
   vfprintf(stderr, fmt, va);
   va_end(va);
}

void retro_init(void)
{
   const char *dir = NULL;

   srand(time(NULL));
   if (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) && dir)
      sprintf(retro_base_directory, "%s", dir);

   if (environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &dir) && dir)
      sprintf(retro_saves_directory, "%s", dir);

   initialize_screnlayout_data(&screen_layout_data);
}

void retro_deinit(void)
{
   return;
}

unsigned retro_api_version(void)
{
   return RETRO_API_VERSION;
}

void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof(*info));
   info->library_name     = "melonDS";
#ifndef GIT_VERSION
#define GIT_VERSION ""
#endif
   info->library_version  = MELONDS_VERSION GIT_VERSION;
   info->need_fullpath    = true;
   info->valid_extensions = "nds";
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   info->timing.fps            = 32.0f * 1024.0f * 1024.0f / 560190.0f;
   info->timing.sample_rate    = 32.0f * 1024.0f;
   info->geometry.base_width   = screen_layout_data.buffer_width;
   info->geometry.base_height  = screen_layout_data.buffer_height;
   info->geometry.max_width    = screen_layout_data.buffer_width;
   info->geometry.max_height   = screen_layout_data.buffer_height;
   info->geometry.aspect_ratio = (float)screen_layout_data.buffer_width / (float)screen_layout_data.buffer_height;
}

void retro_set_environment(retro_environment_t cb)
{
   struct retro_vfs_interface_info vfs_iface_info;
   environ_cb = cb;

#ifdef HAVE_OPENGL
   std::string opengl_resolution = "OpenGL Internal Resolution; ";

   static const int MAX_SCALE = 8;

   char temp[100];
   for(int i = 1; i <= MAX_SCALE; i++)
   {
      temp[0] = 0;
      snprintf(temp, sizeof(temp), "%ix native (%ix%i)", i, VIDEO_WIDTH * i, VIDEO_HEIGHT * i);
      std::string param = temp;

      opengl_resolution.append(param);

      if(i != MAX_SCALE)
         opengl_resolution.append("|");
   }
#endif

#ifdef JIT_ENABLED
   std::string jit_blocksize = "JIT block size; ";

   static const int MAX_JIT_BLOCKSIZE = 100;
   static const int DEFAULT_BLOCK_SIZE = 32;

   jit_blocksize.append(std::to_string(DEFAULT_BLOCK_SIZE) + "|");

   for(int i = 1; i <= MAX_JIT_BLOCKSIZE; i++)
   {
      if(i == DEFAULT_BLOCK_SIZE) continue;

      jit_blocksize.append(std::to_string(i));

      if(i != MAX_JIT_BLOCKSIZE)
         jit_blocksize.append("|");
   }
#endif

  static const retro_variable values[] =
   {
      { "melonds_boot_directly", "Boot game directly; enabled|disabled" },
      { "melonds_screen_layout", "Screen Layout; Top/Bottom|Bottom/Top|Left/Right|Right/Left|Top Only|Bottom Only|Hybrid Top|Hybrid Bottom" },
      { "melonds_hybrid_ratio", "Hybrid ratio; 2|3" },
      { "melonds_swapscreen_mode", "Swap Screen mode; Toggle|Hold" },
#ifdef HAVE_THREADS
      { "melonds_threaded_renderer", "Threaded software renderer; disabled|enabled" },
#endif
      { "melonds_touch_mode", "Touch mode; disabled|Mouse|Touch|Joystick" },
#ifdef HAVE_OPENGL
      { "melonds_opengl_renderer", "OpenGL Renderer (Restart); disabled|enabled" },
      { "melonds_opengl_resolution", opengl_resolution.c_str() },
#endif
#ifdef JIT_ENABLED
      { "melonds_jit_enable", "JIT Enable (Restart); enabled|disabled" },
      { "melonds_jit_block_size", jit_blocksize.c_str() },
      { "melonds_jit_branch_optimisations", "JIT Branch optimisations; enabled|disabled" },
      { "melonds_jit_literal_optimisations", "JIT Literal optimisations; enabled|disabled" },
#endif
      { 0, 0 }
   };

   environ_cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void*)values);

   if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging))
      log_cb = logging.log;
   else
      log_cb = fallback_log;

   static const struct retro_controller_description controllers[] = {
      { "Nintendo DS", RETRO_DEVICE_JOYPAD },
      { NULL, 0 },
   };

   static const struct retro_controller_info ports[] = {
      { controllers, 1 },
      { NULL, 0 },
   };

   cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void*)ports);

   vfs_iface_info.required_interface_version = FILESTREAM_REQUIRED_VFS_VERSION;
   vfs_iface_info.iface = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VFS_INTERFACE, &vfs_iface_info))
      filestream_vfs_init(&vfs_iface_info);
}

void retro_set_audio_sample(retro_audio_sample_t cb)
{
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
   audio_cb = cb;
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

void retro_set_controller_port_device(unsigned port, unsigned device)
{
   log_cb(RETRO_LOG_INFO, "Plugging device %u into port %u.\n", device, port);
}

void retro_reset(void)
{
   NDS::Reset();
   NDS::LoadROM(rom_path.c_str(), save_path.c_str(), direct_boot);
}

static void check_variables(bool init)
{
   struct retro_variable var = {0};

   var.key = "melonds_boot_directly";
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "disabled"))
         direct_boot = false;
      else
         direct_boot = true;
   }

   ScreenLayout layout = ScreenLayout::TopBottom;
   var.key = "melonds_screen_layout";
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "Top/Bottom"))
         layout = ScreenLayout::TopBottom;
      else if (!strcmp(var.value, "Bottom/Top"))
         layout = ScreenLayout::BottomTop;
      else if (!strcmp(var.value, "Left/Right"))
         layout = ScreenLayout::LeftRight;
      else if (!strcmp(var.value, "Right/Left"))
         layout = ScreenLayout::RightLeft;
      else if (!strcmp(var.value, "Top Only"))
         layout = ScreenLayout::TopOnly;
      else if (!strcmp(var.value, "Bottom Only"))
         layout = ScreenLayout::BottomOnly;
      else if (!strcmp(var.value, "Hybrid Top"))
         layout = ScreenLayout::HybridTop;
      else if (!strcmp(var.value, "Hybrid Bottom"))
         layout = ScreenLayout::HybridBottom;
   }

   var.key = "melonds_hybrid_ratio";
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value != NULL)
   {
      screen_layout_data.hybrid_ratio = std::stoi(var.value);
   }

   var.key = "melonds_swapscreen_mode";
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value != NULL)
   {
      toggle_swap_screen = !strcmp(var.value, "Toggle");
   }

#ifdef PORTANDROID
	/* Toggle item from game menu */
	var.key = "menuItemToggleLayout";
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
	{
		printf_1("[%s] menuItemToggleLayout", __FUNCTION__);
		switch(current_screen_layout){
			case ScreenLayout::TopBottom:
				layout = ScreenLayout::LeftRight;
				break;
			case ScreenLayout::BottomTop:
				layout = ScreenLayout::RightLeft;
				break;
			case ScreenLayout::LeftRight:
				layout = ScreenLayout::TopBottom;
				break;
			case ScreenLayout::RightLeft:
				layout = ScreenLayout::BottomTop;
				break;
		}
	}

	var.key = "menuItemSwapScreen";
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
	{
		printf_1("[%s] menuItemSwapScreen", __FUNCTION__);
		switch(current_screen_layout){
			case ScreenLayout::TopBottom:
				layout = ScreenLayout::BottomTop;
				break;
			case ScreenLayout::BottomTop:
				layout = ScreenLayout::TopBottom;
				break;
			case ScreenLayout::LeftRight:
				layout = ScreenLayout::RightLeft;
				break;
			case ScreenLayout::RightLeft:
				layout = ScreenLayout::LeftRight;
				break;
			case ScreenLayout::TopOnly:
				layout = ScreenLayout::BottomOnly;
				break;
			case ScreenLayout::BottomOnly:
				layout = ScreenLayout::TopOnly;
				break;
		}
	}

	/* Toggle Item from front-end   */
#endif

#ifdef HAVE_THREADS
   var.key = "melonds_threaded_renderer";
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "enabled"))
         video_settings.Soft_Threaded = true;
      else
         video_settings.Soft_Threaded = false;
   }
#endif

   TouchMode new_touch_mode = TouchMode::Disabled;

   var.key = "melonds_touch_mode";
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "Mouse"))
         new_touch_mode = TouchMode::Mouse;
      else if (!strcmp(var.value, "Touch"))
         new_touch_mode = TouchMode::Touch;
      else if (!strcmp(var.value, "Joystick"))
         new_touch_mode = TouchMode::Joystick;
   }

#ifdef HAVE_OPENGL
   bool gl_update = false;

   if(input_state.current_touch_mode != new_touch_mode) // Hide the cursor
      gl_update = true;

   // TODO: Fix the OpenGL software only render impl so you can switch at runtime
   if (init)
   {
      var.key = "melonds_opengl_renderer";
      if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      {
         bool use_opengl = !strcmp(var.value, "enabled");

         if(!init && using_opengl) current_renderer = use_opengl ? CurrentRenderer::OpenGL : CurrentRenderer::Software;

         enable_opengl = use_opengl;
      }
   }

   // Running the software rendering thread at the same time as OpenGL is used will cause segfaulty on cleanup
   if(enable_opengl) video_settings.Soft_Threaded = false;

   var.key = "melonds_opengl_resolution";
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      int first_char_val = (int)var.value[0];
      int scaleing = Clamp(first_char_val - 48, 0, 8);

      if(video_settings.GL_ScaleFactor != scaleing)
         gl_update = true;

      video_settings.GL_ScaleFactor = scaleing;
   }
   else
   {
      video_settings.GL_ScaleFactor = 1;
   }

   if((using_opengl && gl_update) || layout != current_screen_layout)
      refresh_opengl = true;
#endif

#ifdef JIT_ENABLED
   var.key = "melonds_jit_enable";
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "enabled"))
         Config::JIT_Enable = true;
      else
         Config::JIT_Enable = false;
   }

   var.key = "melonds_jit_block_size";
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      Config::JIT_MaxBlockSize = std::stoi(var.value);
   }

   var.key = "melonds_jit_branch_optimisations";
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "enabled"))
         Config::JIT_BranchOptimisations = true;
      else
         Config::JIT_BranchOptimisations = false;
   }

   var.key = "melonds_jit_literal_optimisations";
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "enabled"))
         Config::JIT_LiteralOptimisations = true;
      else
         Config::JIT_LiteralOptimisations = false;
   }
#endif

   input_state.current_touch_mode = new_touch_mode;

   update_screenlayout(layout, &screen_layout_data, enable_opengl, swapped_screens);
}

static void audio_callback(void)
{
   static int16_t buffer[0x1000];
   u32 size = SPU::GetOutputSize();
   if(size > sizeof(buffer) / (2 * sizeof(int16_t)))
      size = sizeof(buffer) / (2 * sizeof(int16_t));

   SPU::ReadOutput(buffer, size);
   audio_cb(buffer, size);
}

static void render_frame(void)
{
   if (current_renderer == CurrentRenderer::None)
   {
 #ifdef HAVE_OPENGL
         if (enable_opengl && using_opengl)
         {
            // Try to initialize opengl, if it failed fallback to software
            if (initialize_opengl()) current_renderer = CurrentRenderer::OpenGL;
            else
            {
               using_opengl = false;
               return;
            }
         }
         else
         {
            if(using_opengl) deinitialize_opengl_renderer();
#endif
            GPU::InitRenderer(false);
            current_renderer = CurrentRenderer::Software;
#ifdef HAVE_OPENGL
         }
#endif
   }
#ifdef HAVE_OPENGL
   if(using_opengl)
   {
      if (current_renderer == CurrentRenderer::Software) render_opengl_frame(true);
      else render_opengl_frame(false);
   }
   else if(!enable_opengl)
   {
   #endif
      int frontbuf = GPU::FrontBuffer;

      if(screen_layout_data.hybrid)
      {
         unsigned primary = screen_layout_data.displayed_layout == ScreenLayout::HybridTop ? 0 : 1;
         unsigned secondary = screen_layout_data.displayed_layout == ScreenLayout::HybridTop ? 1 : 0;

         copy_screen(&screen_layout_data, GPU::Framebuffer[frontbuf][primary], screen_layout_data.top_screen_offset, true);
         copy_screen(&screen_layout_data, GPU::Framebuffer[frontbuf][secondary], screen_layout_data.bottom_screen_offset, false);

         if(cursor_enabled(&input_state))
            draw_cursor(&screen_layout_data, input_state.touch_x, input_state.touch_y);

         video_cb((uint8_t*)screen_layout_data.buffer_ptr, screen_layout_data.buffer_width, screen_layout_data.buffer_height, screen_layout_data.buffer_width * sizeof(uint32_t));
      }
      else
      {
         if(screen_layout_data.enable_top_screen)
            copy_screen(&screen_layout_data, GPU::Framebuffer[frontbuf][0], screen_layout_data.top_screen_offset, false);
         if(screen_layout_data.enable_bottom_screen)
            copy_screen(&screen_layout_data, GPU::Framebuffer[frontbuf][1], screen_layout_data.bottom_screen_offset, false);

         if(cursor_enabled(&input_state) && current_screen_layout != ScreenLayout::TopOnly)
            draw_cursor(&screen_layout_data, input_state.touch_x, input_state.touch_y);

         video_cb((uint8_t*)screen_layout_data.buffer_ptr, screen_layout_data.buffer_width, screen_layout_data.buffer_height, screen_layout_data.buffer_width * sizeof(uint32_t));
      }
#ifdef HAVE_OPENGL
   }
#endif
}

void retro_run(void)
{
   update_input(&input_state);

   if (input_state.swap_screens_btn != swapped_screens)
   {
      if (toggle_swap_screen)
      {
         if (swapped_screens == false)
         {
            swap_screen_toggled = !swap_screen_toggled;
            update_screenlayout(current_screen_layout, &screen_layout_data, enable_opengl, swap_screen_toggled);
            refresh_opengl = true;
         }

         swapped_screens = input_state.swap_screens_btn; 
      }
      else
      {
         swapped_screens = input_state.swap_screens_btn; 
         update_screenlayout(current_screen_layout, &screen_layout_data, enable_opengl, swapped_screens);
         refresh_opengl = true;
      }
   }

   if (input_state.holding_noise_btn)
   {
      s16 tmp[735];
      for (int i = 0; i < 735; i++) tmp[i] = rand() & 0xFFFF;
      NDS::MicInputFrame(tmp, 735);
   }
   else
   {
      NDS::MicInputFrame(NULL, 0);
   }

   if (current_renderer != CurrentRenderer::None) NDS::RunFrame();

   render_frame();

   audio_callback();

   bool updated = false;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
   {
      check_variables(false);

      struct retro_system_av_info updated_av_info;
      retro_get_system_av_info(&updated_av_info);
      environ_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &updated_av_info);
   }
}

bool retro_load_game(const struct retro_game_info *info)
{
   std::vector <std::string> required_roms = {"bios7.bin", "bios9.bin", "firmware.bin"};
   std::vector <std::string> missing_roms;

   // Check if any of the bioses / firmware files are missing
   for(std::string& rom : required_roms)
   {
      if(!Platform::LocalFileExists(rom.c_str()))
      {
         missing_roms.push_back(rom);
      }
   }

   // Abort if there are any of the required roms are missing
   if(!missing_roms.empty())
   {
      std::string msg = "Missing required bios/firmware in system directory: ";

      int i = 0;
      int len = missing_roms.size();
      for (auto missing_rom : missing_roms)
      {
         msg.append(missing_rom);
         if(len - 1 > i) msg.append(", ");
         i ++;
      }

      msg.append("\n");

      log_cb(RETRO_LOG_ERROR, msg.c_str());

      return false;
   }

   strcpy(Config::BIOS7Path, "bios7.bin");
   strcpy(Config::BIOS9Path, "bios9.bin");
   strcpy(Config::FirmwarePath, "firmware.bin");

   struct retro_input_descriptor desc[] = {
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "Left" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "Up" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "Down" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Right" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "A" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "B" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "R" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "L" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X, "X" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "Y" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2, "Make microphone noise" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2, "Swap screens" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3, "Close lid" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3, "Touch joystick" },
      { 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Touch joystick X" },
      { 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Touch joystick Y" },
      { 0 },
   };

   environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);

   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
   {
      log_cb(RETRO_LOG_INFO, "XRGB8888 is not supported.\n");
      return false;
   }

   check_variables(true);

   // Initialize the opengl state if needed
#ifdef HAVE_OPENGL
   if (enable_opengl)
      initialize_opengl();
#endif

   if(!NDS::Init())
      return false;

   char game_name[256];
   fill_pathname_base_noext(game_name, info->path, sizeof(game_name));

   rom_path = std::string(info->path);
   save_path = std::string(retro_saves_directory) + std::string(1, PLATFORM_DIR_SEPERATOR) + std::string(game_name) + ".sav";

   GPU::SetRenderSettings(false, video_settings);
   NDS::SetConsoleType(0);
   NDS::LoadROM(rom_path.c_str(), save_path.c_str(), direct_boot);

   (void)info;

   return true;
}

void retro_unload_game(void)
{
   NDS::DeInit();
}

unsigned retro_get_region(void)
{
   return RETRO_REGION_NTSC;
}

bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num)
{
   return false;
}

#define MAX_SERIALIZE_TEST_SIZE 16 * 1024 * 1024 // The current savestate is around 7MiB so 16MiB should be enough for now

size_t retro_serialize_size(void)
{
   // Create the dummy savestate
   void* data = malloc(MAX_SERIALIZE_TEST_SIZE);
   Savestate* savestate = new Savestate(data, MAX_SERIALIZE_TEST_SIZE, true);
   NDS::DoSavestate(savestate);
   // Find the offset to find the current static filesize
   size_t size = savestate->GetOffset();
   // Free
   delete savestate;
   free(data);

   return size;
}

bool retro_serialize(void *data, size_t size)
{
   Savestate* savestate = new Savestate(data, size, true);
   NDS::DoSavestate(savestate);
   delete savestate;

   return true;
}

bool retro_unserialize(const void *data, size_t size)
{
   Savestate* savestate = new Savestate((void*)data, size, false);
   NDS::DoSavestate(savestate);
   delete savestate;

   return true;
}

void *retro_get_memory_data(unsigned type)
{
   if (type == RETRO_MEMORY_SYSTEM_RAM)
      return NDS::MainRAM;
   else
      return NULL;
}

size_t retro_get_memory_size(unsigned type)
{
   if (type == RETRO_MEMORY_SYSTEM_RAM)
      return 0x400000;
   else
      return 0;
}

void retro_cheat_reset(void)
{}

void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
   (void)index;
   (void)enabled;
   (void)code;
}
