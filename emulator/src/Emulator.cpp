#include "Emulator.hpp"

#include "Chipset.hpp"
#include "Logger.hpp"

#include <iostream>
#include <fstream>
#include <sstream>

namespace casioemu
{
	Emulator::Emulator(std::string _model_path, Uint32 _timer_interval, Uint32 _cycles_per_second, bool _paused) : paused(_paused), cycles(_cycles_per_second), chipset(*new Chipset(*this))
	{
		std::lock_guard<std::mutex> access_guard(access_lock);
		running = true;
		timer_interval = _timer_interval;
		model_path = _model_path;

		lua_state = luaL_newstate();
		luaL_openlibs(lua_state);

		LoadModelDefition();

		window = SDL_CreateWindow(
			std::string(GetModelInfo("model_name")).c_str(),
			SDL_WINDOWPOS_UNDEFINED,
			SDL_WINDOWPOS_UNDEFINED,
			int(GetModelInfo("interface_width")),
			int(GetModelInfo("interface_height")),
			SDL_WINDOW_SHOWN
		);
		if (!window)
			PANIC("SDL_CreateWindow failed: %s\n", SDL_GetError());

		chipset.SetupInternals();

		window_surface = SDL_GetWindowSurface(window);

		LoadInterfaceImage();

		SDL_FillRect(window_surface, nullptr, SDL_MapRGB(window_surface->format, 255, 255, 255));
		SDL_BlitSurface(interface_image_surface, nullptr, window_surface, nullptr);
		SDL_UpdateWindowSurface(window);

		cycles.Reset();

		timer_id = SDL_AddTimer(timer_interval, [](Uint32 delay, void *param) {
			Emulator *emulator = (Emulator *)param;
			emulator->TimerCallback();
			return emulator->timer_interval;
		}, this);

		chipset.Reset();
	}

	Emulator::~Emulator()
	{
		std::lock_guard<std::mutex> access_guard(access_lock);
		SDL_RemoveTimer(timer_id);

	    SDL_FreeSurface(interface_image_surface);
		SDL_DestroyWindow(window);

		luaL_unref(lua_state, LUA_REGISTRYINDEX, lua_model_ref);
		lua_close(lua_state);
		delete &chipset;
	}

	void Emulator::LoadModelDefition()
	{
		if (luaL_loadfile(lua_state, (model_path + "/model.lua").c_str()) != LUA_OK)
			PANIC("LoadModelDefition failed: %s\n", lua_tostring(lua_state, -1));

		if (lua_pcall(lua_state, 0, 1, 0) != LUA_OK)
			PANIC("LoadModelDefition failed: %s\n", lua_tostring(lua_state, -1));

		lua_model_ref = luaL_ref(lua_state, LUA_REGISTRYINDEX);
	}

	Emulator::ModelInfo Emulator::GetModelInfo(std::string key)
	{
		return ModelInfo(*this, key);
	}

	Emulator::ModelInfo::ModelInfo(Emulator &_emulator, std::string _key) : emulator(_emulator)
	{
		key = _key;
	}

	Emulator::ModelInfo::operator std::string()
	{
		lua_rawgeti(emulator.lua_state, LUA_REGISTRYINDEX, emulator.lua_model_ref);
		lua_getfield(emulator.lua_state, -1, key.c_str());
		const char *value = lua_tostring(emulator.lua_state, -1);
		if (!value)
			PANIC("ModelInfo::operator std::string failed: key '%s' is not defined\n", key.c_str());
		lua_pop(emulator.lua_state, 2);
		return std::string(value);
	}

	Emulator::ModelInfo::operator int()
	{
		int result;
		std::stringstream ss;
		ss << std::string(*this);
		ss >> result;
		if (ss.fail())
			PANIC("ModelInfo::operator int failed: key '%s' is not convertible to int\n", key.c_str());
		return result;
	}

	void Emulator::LoadInterfaceImage()
	{
	    SDL_Surface *loaded_surface = IMG_Load(GetModelFilePath(GetModelInfo("interface_image_path")).c_str());
	    if (!loaded_surface)
	    	PANIC("IMG_Load failed: %s\n", IMG_GetError());

	    interface_image_surface = SDL_ConvertSurface(loaded_surface, window_surface->format, 0);
	    if (!interface_image_surface)
	    	PANIC("SDL_ConvertSurface failed: %s\n", SDL_GetError());

	    SDL_FreeSurface(loaded_surface);
	}

	std::string Emulator::GetModelFilePath(std::string relative_path)
	{
		return model_path + "/" + relative_path;
	}

	void Emulator::TimerCallback()
	{
		std::lock_guard<std::mutex> access_guard(access_lock);
		Uint64 cycles_to_emulate = cycles.GetDelta();
		for (Uint64 ix = 0; ix != cycles_to_emulate; ++ix)
			chipset.Tick();
	}

	bool Emulator::Running()
	{
		return running;
	}

	bool Emulator::GetPaused()
	{
		return paused;
	}

	void Emulator::Shutdown()
	{
		running = false;
	}

	bool Emulator::ExecuteCommand(std::string command)
	{
		command_buffer.append(command);

		if (luaL_loadstring(lua_state, command_buffer.c_str()) != LUA_OK)
		{
			if (!strstr(lua_tostring(lua_state, -1), "<eof>"))
			{
				command_buffer.clear();
				logger::Info("[Console input] %s\n", lua_tostring(lua_state, -1));
			}

			lua_pop(lua_state, 1);
			return command_buffer.empty();
		}

		command_buffer.clear();
		if (lua_pcall(lua_state, 0, 0, 0) != LUA_OK)
		{
			logger::Info("[Console input] %s\n", lua_tostring(lua_state, -1));
			lua_pop(lua_state, 1);
			return true;
		}

		return true;
	}

	void Emulator::SetPaused(bool _paused)
	{
		paused = _paused;
	}

	Emulator::Cycles::Cycles(Uint64 _cycles_per_second)
	{
		cycles_per_second = _cycles_per_second;
		performance_frequency = SDL_GetPerformanceFrequency();
	}

	void Emulator::Cycles::Reset()
	{
		ticks_at_reset = SDL_GetPerformanceCounter();
		cycles_emulated = 0;
	}

	Uint64 Emulator::Cycles::GetDelta()
	{
		Uint64 ticks_now = SDL_GetPerformanceCounter();
		Uint64 cycles_to_have_been_emulated_by_now = (double)(ticks_now - ticks_at_reset) / performance_frequency * cycles_per_second;
		Uint64 diff = cycles_to_have_been_emulated_by_now - cycles_emulated;
		cycles_emulated = cycles_to_have_been_emulated_by_now;
		return diff;
	}
}
