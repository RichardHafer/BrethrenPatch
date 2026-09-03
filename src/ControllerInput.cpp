#include "Common.hpp"

#define SDL_MAIN_HANDLED
#define DIRECTINPUT_VERSION 0x0800
#include <SDL3/SDL.h>
#include <Xinput.h>
#include <dinput.h>
#include <algorithm>
#include <cmath>
#include <intrin.h>

namespace
{
    safetyhook::InlineHook getStateHook;
    safetyhook::InlineHook setStateHook;
    safetyhook::InlineHook directInputCreateHook;
    safetyhook::InlineHook enumDevicesHook;
    safetyhook::InlineHook createDeviceHook;
    safetyhook::InlineHook mouseGetStateHook;
    safetyhook::InlineHook mouseGetDataHook;

    SDL_Gamepad* gamepad = nullptr;
    DWORD packetNumber = 0;
    XINPUT_GAMEPAD lastState{};
    float smoothYaw = 0.0f;
    float smoothPitch = 0.0f;
    bool gyroAvailable = false;
    thread_local bool insideXInput = false;
    SRWLOCK controllerLock = SRWLOCK_INIT;
    volatile LONG mouseStateSamples = 0;
    volatile LONG mouseDataSamples = 0;
    volatile LONG mouseDeviceCount = 0;
    IDirectInputDevice8A* mouseDevices[8]{};

    using DirectInput8CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD,
                                                  const GUID*, LPVOID*,
                                                  LPUNKNOWN);
    using CreateDeviceFn = HRESULT(WINAPI*)(IDirectInput8A*, const GUID*,
                                            IDirectInputDevice8A**,
                                            IUnknown*);

    void FormatInputCaller(void* caller, char* output, size_t outputSize)
    {
        MEMORY_BASIC_INFORMATION memory{};
        const uintptr_t address = reinterpret_cast<uintptr_t>(caller);
        const uintptr_t moduleBase = VirtualQuery(caller, &memory,
            sizeof(memory))
            ? reinterpret_cast<uintptr_t>(memory.AllocationBase) : 0;
        char path[MAX_PATH]{};
        const char* name = "unknown";
        if (moduleBase && GetModuleFileNameA(
                reinterpret_cast<HMODULE>(moduleBase), path, MAX_PATH))
        {
            const char* slash = std::strrchr(path, '\\');
            name = slash ? slash + 1 : path;
        }
        const uintptr_t offset = moduleBase ? address - moduleBase : address;
        std::snprintf(output, outputSize, "%s+%08X", name,
                      static_cast<unsigned>(offset));
    }

    void* ReadMousePollParent(uintptr_t returnSlot)
    {
        constexpr uintptr_t kPollFrameParentOffset = 0x14C;
        const uintptr_t parentSlot = returnSlot + kPollFrameParentOffset;

        MEMORY_BASIC_INFORMATION memory{};
        if (!VirtualQuery(reinterpret_cast<const void*>(parentSlot), &memory,
                          sizeof(memory)) ||
            memory.State != MEM_COMMIT ||
            (memory.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            return nullptr;

        const uintptr_t regionEnd =
            reinterpret_cast<uintptr_t>(memory.BaseAddress) + memory.RegionSize;
        if (parentSlot > regionEnd || sizeof(uintptr_t) > regionEnd - parentSlot)
            return nullptr;

        return *reinterpret_cast<void* const*>(parentSlot);
    }

    bool IsTrackedMouse(IDirectInputDevice8A* device)
    {
        const LONG count = InterlockedCompareExchange(&mouseDeviceCount, 0, 0);
        for (LONG i = 0; i < count; ++i)
            if (mouseDevices[i] == device)
                return true;
        return false;
    }

    void TrackMouse(IDirectInputDevice8A* device)
    {
        if (!device || IsTrackedMouse(device))
            return;
        const LONG slot = InterlockedIncrement(&mouseDeviceCount) - 1;
        if (slot < 8)
            mouseDevices[slot] = device;
        else
            InterlockedDecrement(&mouseDeviceCount);
    }

    HRESULT WINAPI MouseGetDeviceStateDetour(IDirectInputDevice8A* self,
                                              DWORD size, void* data)
    {
        const HRESULT result = mouseGetStateHook.stdcall<HRESULT>(
            self, size, data);
        if (SUCCEEDED(result) && IsTrackedMouse(self) && data &&
            size >= sizeof(DIMOUSESTATE))
        {
            const auto* state = static_cast<const DIMOUSESTATE*>(data);
            if (state->lX || state->lY || state->lZ)
            {
                const LONG sample = InterlockedIncrement(&mouseStateSamples);
                if (sample <= 32)
                {
                    char callers[512]{};
                    char pollParent[128]{};
                    FormatInputCaller(_ReturnAddress(), callers,
                                      sizeof(callers));
                    FormatInputCaller(ReadMousePollParent(
                                          reinterpret_cast<uintptr_t>(
                                              _AddressOfReturnAddress())),
                                      pollParent,
                                      sizeof(pollParent));
                    LogNow("Mouse GetDeviceState %ld: size=%lu delta=(%ld,%ld,%ld), caller=%s poll-parent=%s",
                           sample, size, state->lX, state->lY, state->lZ,
                           callers, pollParent);
                }
            }
        }
        return result;
    }

    HRESULT WINAPI MouseGetDeviceDataDetour(IDirectInputDevice8A* self,
                                             DWORD objectSize,
                                             DIDEVICEOBJECTDATA* objects,
                                             DWORD* count, DWORD flags)
    {
        const HRESULT result = mouseGetDataHook.stdcall<HRESULT>(
            self, objectSize, objects, count, flags);
        if (SUCCEEDED(result) && IsTrackedMouse(self) && objects && count &&
            *count)
        {
            const LONG sample = InterlockedIncrement(&mouseDataSamples);
            if (sample <= 32)
            {
                char callers[512]{};
                FormatInputCaller(_ReturnAddress(), callers, sizeof(callers));
                const DIDEVICEOBJECTDATA& first = objects[0];
                LogNow("Mouse GetDeviceData %ld: records=%lu objectSize=%lu first=(ofs=%lu data=%ld), caller=%s",
                       sample, *count, objectSize, first.dwOfs,
                       static_cast<LONG>(first.dwData), callers);
            }
        }
        return result;
    }

    bool IsMouseGuid(const GUID* guid)
    {
        return guid &&
            (IsEqualGUID(*guid, GUID_SysMouse) ||
             IsEqualGUID(*guid, GUID_SysMouseEm) ||
             IsEqualGUID(*guid, GUID_SysMouseEm2));
    }

    HRESULT WINAPI CreateDeviceDetour(IDirectInput8A* self, const GUID* guid,
                                       IDirectInputDevice8A** output,
                                       IUnknown* outer)
    {
        const HRESULT result = createDeviceHook.original<CreateDeviceFn>()(
            self, guid, output, outer);
        if (SUCCEEDED(result) && output && *output && IsMouseGuid(guid) &&
            cfg::rawMouseInputDiagnostics)
        {
            TrackMouse(*output);
            void** table = *reinterpret_cast<void***>(*output);
            if (!mouseGetStateHook)
                mouseGetStateHook = Hook(table[9],
                    reinterpret_cast<void*>(&MouseGetDeviceStateDetour));
            if (!mouseGetDataHook)
                mouseGetDataHook = Hook(table[10],
                    reinterpret_cast<void*>(&MouseGetDeviceDataDetour));
            Log("Mouse DirectInput device: GetDeviceState %s, GetDeviceData %s",
                mouseGetStateHook ? "hooked" : "NOT hooked",
                mouseGetDataHook ? "hooked" : "NOT hooked");
        }
        return result;
    }

    struct EnumContext
    {
        LPDIENUMDEVICESCALLBACKA callback;
        void* user;
    };

    BOOL CALLBACK FilteredDeviceCallback(const DIDEVICEINSTANCEA* device,
                                          void* context)
    {
        auto* filter = static_cast<EnumContext*>(context);
        const DWORD type = GET_DIDEVICE_TYPE(device->dwDevType);
        const bool isGameController =
            type == DI8DEVTYPE_JOYSTICK ||
            type == DI8DEVTYPE_GAMEPAD ||
            type == DI8DEVTYPE_DRIVING ||
            type == DI8DEVTYPE_FLIGHT ||
            type == DI8DEVTYPE_1STPERSON ||
            type == DI8DEVTYPE_DEVICECTRL;
        if (isGameController)
        {
            if (cfg::sdlControllerDiagnostics)
                Log("Input filter: ignored DirectInput device %s",
                    device->tszProductName);
            return DIENUM_CONTINUE;
        }
        return filter->callback(device, filter->user);
    }

    HRESULT WINAPI EnumDevicesDetour(IDirectInput8A* self, DWORD type,
                                      LPDIENUMDEVICESCALLBACKA callback,
                                      void* user, DWORD flags)
    {
        if (!callback)
            return enumDevicesHook.stdcall<HRESULT>(self, type, callback,
                                                    user, flags);
        EnumContext context{callback, user};
        return enumDevicesHook.stdcall<HRESULT>(
            self, type, FilteredDeviceCallback, &context, flags);
    }

    HRESULT WINAPI DirectInput8CreateDetour(HINSTANCE instance, DWORD version,
                                             const GUID* iid, LPVOID* output,
                                             LPUNKNOWN outer)
    {
        const HRESULT result = directInputCreateHook
            .original<DirectInput8CreateFn>()(
                instance, version, iid, output, outer);
        const bool needEnumHook = cfg::filterInputDevices && !enumDevicesHook;
        const bool needCreateHook = cfg::rawMouseInputDiagnostics &&
                                    !createDeviceHook;
        if (SUCCEEDED(result) && output && *output &&
            (needEnumHook || needCreateHook))
        {
            void** table = *reinterpret_cast<void***>(*output);
            if (cfg::filterInputDevices)
            {
                enumDevicesHook = Hook(table[4],
                    reinterpret_cast<void*>(&EnumDevicesDetour));
                Log("Input filter: DirectInput EnumDevices %s",
                    enumDevicesHook ? "hooked" : "NOT hooked");
            }
            if (cfg::rawMouseInputDiagnostics)
            {
                createDeviceHook = Hook(table[3],
                    reinterpret_cast<void*>(&CreateDeviceDetour));
                Log("Mouse diagnostics: DirectInput CreateDevice %s",
                    createDeviceHook ? "hooked" : "NOT hooked");
            }
        }
        return result;
    }

    void CloseGamepad()
    {
        if (gamepad)
            SDL_CloseGamepad(gamepad);
        gamepad = nullptr;
        gyroAvailable = false;
        smoothYaw = 0.0f;
        smoothPitch = 0.0f;
    }

    bool OpenGamepad()
    {
        if (gamepad && SDL_GamepadConnected(gamepad))
            return true;
        CloseGamepad();

        int count = 0;
        SDL_JoystickID* devices = SDL_GetGamepads(&count);
        if (!devices || count < 1)
        {
            SDL_free(devices);
            return false;
        }

        gamepad = SDL_OpenGamepad(devices[0]);
        SDL_free(devices);
        if (!gamepad)
            return false;

        gyroAvailable = SDL_GamepadHasSensor(gamepad, SDL_SENSOR_GYRO);
        if (gyroAvailable)
            gyroAvailable = SDL_SetGamepadSensorEnabled(
                gamepad, SDL_SENSOR_GYRO, true);

        if (cfg::sdlControllerDiagnostics)
            Log("SDL controller: opened %s, gyro %s",
                SDL_GetGamepadName(gamepad),
                gyroAvailable ? "available" : "unavailable");
        return true;
    }

    SHORT NegateAxis(Sint16 value)
    {
        return value == -32768 ? 32767 : static_cast<SHORT>(-value);
    }

    SHORT ClampAxis(int value)
    {
        return static_cast<SHORT>(std::clamp(value, -32768, 32767));
    }

    void AddGyro(XINPUT_GAMEPAD& state)
    {
        if (!cfg::gyroAiming || !gyroAvailable)
            return;
        if (cfg::gyroAimOnly &&
            state.bLeftTrigger < XINPUT_GAMEPAD_TRIGGER_THRESHOLD)
            return;

        float gyro[3]{};
        if (!SDL_GetGamepadSensorData(gamepad, SDL_SENSOR_GYRO, gyro, 3))
            return;

        const float smoothing = std::clamp(cfg::gyroSmoothing, 0.0f, 1.0f);
        smoothPitch += (gyro[0] - smoothPitch) * (1.0f - smoothing);
        smoothYaw += (gyro[1] - smoothYaw) * (1.0f - smoothing);

        constexpr float radiansToStick = 7200.0f;
        const int yaw = static_cast<int>(
            -smoothYaw * cfg::gyroSensitivity * radiansToStick);
        const int pitch = static_cast<int>(
            -smoothPitch * cfg::gyroSensitivity * radiansToStick);
        state.sThumbRX = ClampAxis(static_cast<int>(state.sThumbRX) + yaw);
        state.sThumbRY = ClampAxis(static_cast<int>(state.sThumbRY) + pitch);
    }

    void MapGamepad(XINPUT_GAMEPAD& state)
    {
        ZeroMemory(&state, sizeof(state));
        struct ButtonMap { SDL_GamepadButton source; WORD target; };
        static constexpr ButtonMap buttons[] = {
            {SDL_GAMEPAD_BUTTON_SOUTH, XINPUT_GAMEPAD_A},
            {SDL_GAMEPAD_BUTTON_EAST, XINPUT_GAMEPAD_B},
            {SDL_GAMEPAD_BUTTON_WEST, XINPUT_GAMEPAD_X},
            {SDL_GAMEPAD_BUTTON_NORTH, XINPUT_GAMEPAD_Y},
            {SDL_GAMEPAD_BUTTON_BACK, XINPUT_GAMEPAD_BACK},
            {SDL_GAMEPAD_BUTTON_START, XINPUT_GAMEPAD_START},
            {SDL_GAMEPAD_BUTTON_LEFT_STICK, XINPUT_GAMEPAD_LEFT_THUMB},
            {SDL_GAMEPAD_BUTTON_RIGHT_STICK, XINPUT_GAMEPAD_RIGHT_THUMB},
            {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, XINPUT_GAMEPAD_LEFT_SHOULDER},
            {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER},
            {SDL_GAMEPAD_BUTTON_DPAD_UP, XINPUT_GAMEPAD_DPAD_UP},
            {SDL_GAMEPAD_BUTTON_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_DOWN},
            {SDL_GAMEPAD_BUTTON_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_LEFT},
            {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT},
        };
        for (const auto& button : buttons)
            if (SDL_GetGamepadButton(gamepad, button.source))
                state.wButtons |= button.target;

        const int leftTrigger = std::max<int>(0,
            SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
        const int rightTrigger = std::max<int>(0,
            SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
        state.bLeftTrigger = static_cast<BYTE>(leftTrigger * 255 / 32767);
        state.bRightTrigger = static_cast<BYTE>(rightTrigger * 255 / 32767);
        state.sThumbLX = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX);
        state.sThumbLY = NegateAxis(
            SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY));
        state.sThumbRX = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTX);
        state.sThumbRY = NegateAxis(
            SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTY));
        AddGyro(state);
    }

    DWORD WINAPI XInputGetStateDetour(DWORD index, XINPUT_STATE* state)
    {
        if (insideXInput)
            return getStateHook.stdcall<DWORD>(index, state);
        if (index != 0 || !state)
            return getStateHook.stdcall<DWORD>(index, state);

        insideXInput = true;
        AcquireSRWLockExclusive(&controllerLock);
        SDL_UpdateGamepads();
        if (!OpenGamepad())
        {
            ReleaseSRWLockExclusive(&controllerLock);
            insideXInput = false;
            return getStateHook.stdcall<DWORD>(index, state);
        }

        XINPUT_GAMEPAD current{};
        MapGamepad(current);
        if (std::memcmp(&current, &lastState, sizeof(current)) != 0)
        {
            lastState = current;
            ++packetNumber;
        }
        state->dwPacketNumber = packetNumber;
        state->Gamepad = current;
        ReleaseSRWLockExclusive(&controllerLock);
        insideXInput = false;
        return ERROR_SUCCESS;
    }

    DWORD WINAPI XInputSetStateDetour(DWORD index,
                                      XINPUT_VIBRATION* vibration)
    {
        if (index != 0)
            return setStateHook.stdcall<DWORD>(index, vibration);
        AcquireSRWLockExclusive(&controllerLock);
        if (!gamepad || !SDL_GamepadConnected(gamepad))
        {
            ReleaseSRWLockExclusive(&controllerLock);
            return setStateHook.stdcall<DWORD>(index, vibration);
        }
        if (vibration)
            SDL_RumbleGamepad(gamepad, vibration->wLeftMotorSpeed,
                              vibration->wRightMotorSpeed, 100);
        ReleaseSRWLockExclusive(&controllerLock);
        return ERROR_SUCCESS;
    }

    void HookXInput()
    {
        static constexpr const wchar_t* candidates[] = {
            L"xinput1_3.dll", L"xinput1_4.dll", L"xinput9_1_0.dll",
            L"xinput1_2.dll", L"xinput1_1.dll"
        };
        for (const wchar_t* name : candidates)
        {
            HMODULE module = GetModuleHandleW(name);
            if (!module)
                continue;
            void* getState = reinterpret_cast<void*>(
                GetProcAddress(module, "XInputGetState"));
            void* setState = reinterpret_cast<void*>(
                GetProcAddress(module, "XInputSetState"));
            if (!getState)
                continue;
            getStateHook = Hook(getState,
                reinterpret_cast<void*>(&XInputGetStateDetour));
            if (setState)
                setStateHook = Hook(setState,
                    reinterpret_cast<void*>(&XInputSetStateDetour));
            Log("SDL controller: XInputGetState %s",
                getStateHook ? "hooked" : "NOT hooked");
            return;
        }
        Log("SDL controller: no loaded XInput module found");
    }
}

void ApplyControllerInput()
{
    if (cfg::filterInputDevices || cfg::rawMouseInputDiagnostics)
    {
        HMODULE module = GetModuleHandleW(L"dinput8.dll");
        if (!module)
            module = LoadLibraryW(L"dinput8.dll");
        void* target = module ? reinterpret_cast<void*>(
            GetProcAddress(module, "DirectInput8Create")) : nullptr;
        if (target)
            directInputCreateHook = Hook(target,
                reinterpret_cast<void*>(&DirectInput8CreateDetour));
        Log("Input filter: DirectInput8Create %s",
            directInputCreateHook ? "hooked" : "NOT hooked");
    }

    if (!cfg::useSdlControllerInput)
        return;

    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD))
    {
        Log("SDL controller: initialization failed: %s", SDL_GetError());
        return;
    }
    HookXInput();
    OpenGamepad();
}
