#pragma once
#include "../helpers/logger.hpp"
#include "../helpers/memory.hpp"
#include "../helpers/simplehook.hpp"

#include "../imgui/imgui.h"
#include "../imgui/imgui_impl_dx9.h"
#include "../imgui/imgui_impl_win32.h"
#include "../immods/imfilebrowser.hpp"
#include "../immods/TextEditor.h"

#include "../globals.hpp"

#include <d3d9.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <filesystem>
#pragma comment(lib, "d3d9.lib")

typedef HRESULT(APIENTRY* PresentFn)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace OverlayHook
{
	inline SimpleHook* hook = nullptr;
	inline HWND gameWindow = nullptr;
	inline WNDPROC originalWndProc = nullptr;
	inline PresentFn originalPresent = nullptr;

	inline bool first_call = true;
	inline ImGui::FileBrowser fileDialog;
	inline TextEditor editor;

	// --- Auto-execute variables ---
	inline std::thread autoExecThread;
	inline std::atomic<bool> autoExecRunning{ false };
	inline std::mutex autoExecMutex;
	inline std::string autoExecScriptPath;
	inline std::string autoExecScriptContent;
	inline int autoExecIntervalSec = 5;
	// --------------------------------

	static void AutoExecLoop()
	{
		while (autoExecRunning.load())
		{
			{
				std::lock_guard<std::mutex> lk(autoExecMutex);
				if (!autoExecScriptContent.empty())
				{
					executor.scriptQueue.push(autoExecScriptContent);
					logs->println("Auto-execute: queued script from %s", autoExecScriptPath.empty() ? "<memory>" : autoExecScriptPath.c_str());
				}
			}

			int total_ms = autoExecIntervalSec * 1000;
			int slept = 0;
			const int step = 100;
			while (autoExecRunning.load() && slept < total_ms)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(step));
				slept += step;
			}
		}
	}

	HRESULT APIENTRY hookedPresent(IDirect3DDevice9* device, const RECT* src_rect, const RECT* dest_rect, HWND dest_window_override, const RGNDATA* dirty_region)
	{
		if (first_call)
		{
			first_call = false;

			ImGui::CreateContext();
			ImGuiIO& io = ImGui::GetIO();

			ImGui_ImplWin32_Init(FindWindowA("Valve001", NULL));
			ImGui_ImplDX9_Init(device);

			fileDialog.SetTitle("Look for Files");
			fileDialog.SetTypeFilters({ ".lua" });

			editor.SetLanguageDefinition(TextEditor::LanguageDefinition::Lua());
		}

		ImGui_ImplDX9_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();

		if (menuOpen)
		{
			ImGui::SetNextWindowSize(ImVec2(800, 600));
			ImGui::Begin("Gmod Lua Executor", &menuOpen, ImGuiWindowFlags_NoSavedSettings);
			ImGui::BeginTabBar("MainTabBar");

			if (ImGui::BeginTabItem("File Browser"))
			{
				if (ImGui::Button("Open File Browser"))
					fileDialog.Open();

				ImGui::EndTabItem();
			}

			if (ImGui::BeginTabItem("Lua Editor"))
			{
				auto cpos = editor.GetCursorPosition();
				ImGui::Text("%6d/%-6d %6d lines  | %s | %s | %s", cpos.mLine + 1, cpos.mColumn + 1, editor.GetTotalLines(),
					editor.IsOverwrite() ? "Ovr" : "Ins",
					editor.CanUndo() ? "*" : " ",
					editor.GetLanguageDefinition().mName.c_str());

				ImGui::SameLine();

				if (ImGui::Button("Execute"))
				{
					auto script = editor.GetText();
					executor.scriptQueue.push(script);
					logs->println("Queued script from editor.");
				}

				editor.Render("TextEditor");

				ImGui::EndTabItem();
			}

			if (ImGui::BeginTabItem("Auto-Execute"))
			{
				ImGui::TextWrapped("Select a Lua file and press Start. The selected script will be queued every N seconds.");

				if (ImGui::Button("Select Script File"))
				{
					fileDialog.Open();
				}
				ImGui::SameLine();
				if (!autoExecScriptPath.empty())
				{
					ImGui::Text("Selected: %s", std::filesystem::path(autoExecScriptPath).filename().string().c_str());
				}
				else
				{
					ImGui::Text("No script selected");
				}

				ImGui::SliderInt("Interval (sec)", &autoExecIntervalSec, 1, 60);

				if (!autoExecRunning.load())
				{
					if (ImGui::Button("Start Auto-Execute"))
					{
						std::lock_guard<std::mutex> lk(autoExecMutex);
						if (autoExecScriptContent.empty())
						{
							logs->println("Auto-execute: no script loaded, cannot start.");
						}
						else
						{
							autoExecRunning.store(true);
							autoExecThread = std::thread(AutoExecLoop);
							logs->println("Auto-execute: started (interval %d sec).", autoExecIntervalSec);
						}
					}
				}
				else
				{
					if (ImGui::Button("Stop Auto-Execute"))
					{
						autoExecRunning.store(false);
						if (autoExecThread.joinable())
							autoExecThread.join();
						logs->println("Auto-execute: stopped.");
					}
				}

				if (ImGui::Button("Execute Now"))
				{
					std::lock_guard<std::mutex> lk(autoExecMutex);
					if (!autoExecScriptContent.empty())
					{
						executor.scriptQueue.push(autoExecScriptContent);
						logs->println("Auto-execute: queued script (manual).");
					}
					else
					{
						logs->println("Auto-execute: no script loaded.");
					}
				}

				ImGui::EndTabItem();
			}

			ImGui::EndTabBar();
			ImGui::End();

			fileDialog.Display();

			if (fileDialog.HasSelected())
			{
				std::ifstream file(fileDialog.GetSelected().string());
				if (file.is_open())
				{
					std::string script((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
					executor.scriptQueue.push(script);
					logs->println("Queued script from file: %s", fileDialog.GetSelected().string().c_str());

					// Also set as auto-exec script (overwrite)
					{
						std::lock_guard<std::mutex> lk(autoExecMutex);
						autoExecScriptPath = fileDialog.GetSelected().string();
						autoExecScriptContent = script;
					}
				}
				else
				{
					logs->println("Failed to open selected file: %s", fileDialog.GetSelected().string().c_str());
				}
				fileDialog.ClearSelected();
			}
		}

		ImGui::EndFrame();
		ImGui::Render();

		ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());

		return originalPresent(device, src_rect, dest_rect, dest_window_override, dirty_region);
	}

	LRESULT WINAPI WndProc(const HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
	{
		switch (uMsg)
		{
		case WM_KEYDOWN:
			if (wParam == VK_SUBTRACT)
				menuOpen ^= 1; // Toggle menu open state with Subtract key
			break;
		}

		// Menu open var
		if (menuOpen)
		{
			ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam);

			ImGuiIO& io = ImGui::GetIO();
			bool is_mouse = (uMsg >= WM_MOUSEFIRST && uMsg <= WM_MOUSELAST);
			bool is_keyboard = (uMsg >= WM_KEYFIRST && uMsg <= WM_KEYLAST);

			if ((is_mouse && io.WantCaptureMouse) || (is_keyboard && io.WantCaptureKeyboard))
				return true; // Block background if ImGui wants input
		}

		return CallWindowProcA(originalWndProc, hWnd, uMsg, wParam, lParam);
	}

	bool Initialize()
	{
		gameWindow = FindWindowA("Valve001", nullptr);
		if (!gameWindow)
		{
			logs->println("Failed to find game window.");
			return false;
		}

		logs->println("Found game window: 0x%p", gameWindow);

		originalWndProc = (WNDPROC)SetWindowLongPtrA(gameWindow, GWLP_WNDPROC, (LONG_PTR)WndProc);
		if (!originalWndProc)
		{
			logs->println("Failed to set new WndProc.");
			return false;
		}

		logs->println("Successfully set new WndProc.");

		auto mod = (uintptr_t)GetModuleHandleA("gameoverlayrenderer64.dll");
		if (!mod)
		{
			logs->println("Failed to get module handle for gameoverlayrenderer64.dll.");
			return false;
		}

		logs->println("Found gameoverlayrenderer64.dll at: 0x%p", (HMODULE)mod);

		auto ptr = Memory::PatternScan(static_cast<uintptr_t>(mod),
			"48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 48 89 7C 24 ? 41 54 41 56 41 57 48 81 EC ? ? ? ? 4C 8B A4 24 ? ? ? ?");

		if (!ptr)
		{
			logs->println("Failed to find pattern in gameoverlayrenderer64.dll.");
			return false;
		}

		logs->println("Found pattern at: 0x%p", (void*)ptr);

		hook = new SimpleHook((void*)ptr);
		hook->hook(&hookedPresent);
		originalPresent = hook->get_original<PresentFn>();

		if (!originalPresent)
		{
			logs->println("Failed to get original Present function.");
			return false;
		}

		logs->println("Successfully hooked Present function.");
		return true;
	}

	bool Cleanup()
	{
		if (autoExecRunning.load())
		{
			autoExecRunning.store(false);
			if (autoExecThread.joinable())
				autoExecThread.join();
			logs->println("Auto-execute: thread stopped in Cleanup.");
		}

		if (hook)
		{
			hook->unhook();
			delete hook;
			hook = nullptr;
			logs->println("Unhooked Present function.");
		}

		if (originalWndProc)
		{
			SetWindowLongPtrA(gameWindow, GWLP_WNDPROC, (LONG_PTR)originalWndProc);
			logs->println("Restored original WndProc.");
		}

		logs->println("OverlayHook cleanup complete.");
		return true;
	}

}
