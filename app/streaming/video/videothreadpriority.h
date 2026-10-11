#pragma once

#include <SDL.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <avrt.h>
#endif

// Construct on a dedicated video thread and keep alive until it exits. SDL
// provides the portable request; Windows additionally budgets multimedia CPU
// time through MMCSS, independently of the selected GPU/rendering API.
class VideoThreadPriority {
public:
    enum class Role { Work, Deadline };

    explicit VideoThreadPriority(const char* name, Role role = Role::Work)
    {
#ifdef _WIN32
        // Load the OS copy only. No additional deployment DLL or import-library
        // dependency, and systems without MMCSS retain the SDL fallback.
        m_Avrt = LoadLibraryExW(L"avrt.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (m_Avrt) {
            const auto registerTask = reinterpret_cast<decltype(&AvSetMmThreadCharacteristicsW)>(
                GetProcAddress(m_Avrt, "AvSetMmThreadCharacteristicsW"));
            const auto setPriority = reinterpret_cast<decltype(&AvSetMmThreadPriority)>(
                GetProcAddress(m_Avrt, "AvSetMmThreadPriority"));
            m_Revert = reinterpret_cast<decltype(&AvRevertMmThreadCharacteristics)>(
                GetProcAddress(m_Avrt, "AvRevertMmThreadCharacteristics"));
            if (registerTask && setPriority && m_Revert) {
                DWORD taskIndex = 0;
                m_Task = registerTask(L"Playback", &taskIndex);
                if (m_Task) {
                    const auto priority = role == Role::Deadline ? AVRT_PRIORITY_HIGH : AVRT_PRIORITY_NORMAL;
                    if (setPriority(m_Task, priority)) {
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                    "Video thread priority: %s MMCSS Playback %s accepted",
                                    name, role == Role::Deadline ? "HIGH" : "NORMAL");
                        return;
                    }
                    // Registration still provides multimedia scheduling when
                    // relative adjustment is denied. Do not overwrite it via SDL.
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                "Video thread priority: %s MMCSS Playback registered; relative request failed (%lu)",
                                name, static_cast<unsigned long>(GetLastError()));
                    return;
                }
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Video thread priority: %s MMCSS unavailable (%lu); trying SDL",
                            name, static_cast<unsigned long>(GetLastError()));
            }
        }
#endif
        requestSdl(name, role);
    }

    ~VideoThreadPriority()
    {
#ifdef _WIN32
        if (m_Task) m_Revert(m_Task);
        if (m_Avrt) FreeLibrary(m_Avrt);
#endif
    }

    VideoThreadPriority(const VideoThreadPriority&) = delete;
    VideoThreadPriority& operator=(const VideoThreadPriority&) = delete;

private:
    static void requestSdl(const char* name, Role role)
    {
#if SDL_VERSION_ATLEAST(2, 0, 9)
        if (role == Role::Deadline) {
            if (SDL_SetThreadPriority(SDL_THREAD_PRIORITY_TIME_CRITICAL) == 0) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Video thread priority: %s SDL TIME_CRITICAL accepted", name);
                return;
            }
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Video thread priority: %s SDL TIME_CRITICAL rejected (%s); trying HIGH",
                        name, SDL_GetError());
        }
#else
        (void) role;
#endif
        if (SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH) == 0) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Video thread priority: %s SDL HIGH accepted", name);
        }
        else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Video thread priority: %s SDL HIGH rejected (%s); existing scheduling retained",
                        name, SDL_GetError());
        }
    }

#ifdef _WIN32
    HMODULE m_Avrt = nullptr;
    HANDLE m_Task = nullptr;
    decltype(&AvRevertMmThreadCharacteristics) m_Revert = nullptr;
#endif
};
