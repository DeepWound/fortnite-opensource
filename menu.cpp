#include "meni.h"
#include "Globals.h"
#include "stdafx.h"

ID3D11Device* device = nullptr;
ID3D11DeviceContext* immediateContext = nullptr;
ID3D11RenderTargetView* renderTargetView = nullptr;

HRESULT(*PresentOriginal)(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) = nullptr;
HRESULT(*ResizeOriginal)(IDXGISwapChain* swapChain, UINT bufferCount, UINT width, UINT height, DXGI_FORMAT newFormat, UINT swapChainFlags) = nullptr;
WNDPROC oWndProc;

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static bool ShowMenu = true;
namespace ImGui
{
	static inline float ObliqueSliderBehaviorCalcRatioFromValue(float v, float v_min, float v_max, float power, float linear_zero_pos)
	{
		if (v_min == v_max)
			return 0.0f;

		const bool is_non_linear = (power < 1.0f - 0.00001f) || (power > 1.0f + 0.00001f);
		const float v_clamped = (v_min < v_max) ? ImClamp(v, v_min, v_max) : ImClamp(v, v_max, v_min);
		if (is_non_linear)
		{
			if (v_clamped < 0.0f)
			{
				const float f = 1.0f - (v_clamped - v_min) / (ImMin(0.0f, v_max) - v_min);
				return (1.0f - powf(f, 1.0f / power)) * linear_zero_pos;
			}
			else
			{
				const float f = (v_clamped - ImMax(0.0f, v_min)) / (v_max - ImMax(0.0f, v_min));
				return linear_zero_pos + powf(f, 1.0f / power) * (1.0f - linear_zero_pos);
			}
		}

		// Linear slider
		return (v_clamped - v_min) / (v_max - v_min);
	}
	IMGUI_API bool ObliqueSliderBehavior(const ImRect& frame_bb, ImGuiID id, float* v, float v_min, float v_max, float power, int decimal_precision, ImGuiSliderFlags flags = 0)
	{
		ImGuiContext& g = *GImGui;
		ImGuiWindow* window = GetCurrentWindow();
		const ImGuiStyle& style = g.Style;

		// Draw frame
		//RenderFrame(frame_bb.Min, frame_bb.Max, GetColorU32(ImGuiCol_FrameBg), true, style.FrameRounding);
		//
		ImRect bbMin(ImVec2(frame_bb.Min.x + 6, frame_bb.Min.y), ImVec2(frame_bb.Min.x, frame_bb.Max.y));
		ImRect bbMax(ImVec2(frame_bb.Max.x, frame_bb.Max.y), ImVec2(frame_bb.Max.x + 6, frame_bb.Min.y));
		window->DrawList->AddQuadFilled(bbMin.Min, bbMin.Max, bbMax.Min, bbMax.Max, GetColorU32(ImGuiCol_FrameBg));

		const bool is_non_linear = (power < 1.0f - 0.00001f) || (power > 1.0f + 0.00001f);
		const bool is_horizontal = (flags & ImGuiSliderFlags_Vertical) == 0;

		const float grab_padding = 0.0f;
		const float slider_sz = is_horizontal ? (frame_bb.GetWidth() - grab_padding * 2.0f) : (frame_bb.GetHeight() - grab_padding * 2.0f);
		float grab_sz;
		if (decimal_precision != 0)
			grab_sz = ImMin(style.GrabMinSize, slider_sz);
		else
			grab_sz = ImMin(ImMax(1.0f * (slider_sz / ((v_min < v_max ? v_max - v_min : v_min - v_max) + 1.0f)), style.GrabMinSize), slider_sz);  // Integer sliders, if possible have the grab size represent 1 unit
		const float slider_usable_sz = slider_sz - grab_sz;
		const float slider_usable_pos_min = (is_horizontal ? frame_bb.Min.x : frame_bb.Min.y) + grab_padding + grab_sz * 0.5f;
		const float slider_usable_pos_max = (is_horizontal ? frame_bb.Max.x : frame_bb.Max.y) - grab_padding - grab_sz * 0.5f;

		// For logarithmic sliders that cross over sign boundary we want the exponential increase to be symmetric around 0.0f
		float linear_zero_pos = 0.0f;   // 0.0->1.0f
		if (v_min * v_max < 0.0f)
		{
			// Different sign
			const float linear_dist_min_to_0 = powf(fabsf(0.0f - v_min), 1.0f / power);
			const float linear_dist_max_to_0 = powf(fabsf(v_max - 0.0f), 1.0f / power);
			linear_zero_pos = linear_dist_min_to_0 / (linear_dist_min_to_0 + linear_dist_max_to_0);
		}
		else
		{
			// Same sign
			linear_zero_pos = v_min < 0.0f ? 1.0f : 0.0f;
		}

		// Process clicking on the slider
		bool value_changed = false;
		if (g.ActiveId == id)
		{
			bool set_new_value = false;
			float clicked_t = 0.0f;
			if (g.IO.MouseDown[0])
			{
				const float mouse_abs_pos = is_horizontal ? g.IO.MousePos.x : g.IO.MousePos.y;
				clicked_t = (slider_usable_sz > 0.0f) ? ImClamp((mouse_abs_pos - slider_usable_pos_min) / slider_usable_sz, 0.0f, 1.0f) : 0.0f;
				if (!is_horizontal)
					clicked_t = 1.0f - clicked_t;
				set_new_value = true;
			}
			else
			{
				ClearActiveID();
			}

			if (set_new_value)
			{
				float new_value;
				if (is_non_linear)
				{
					// Account for logarithmic scale on both sides of the zero
					if (clicked_t < linear_zero_pos)
					{
						// Negative: rescale to the negative range before powering
						float a = 1.0f - (clicked_t / linear_zero_pos);
						a = powf(a, power);
						new_value = ImLerp(ImMin(v_max, 0.0f), v_min, a);
					}
					else
					{
						// Positive: rescale to the positive range before powering
						float a;
						if (fabsf(linear_zero_pos - 1.0f) > 1.e-6f)
							a = (clicked_t - linear_zero_pos) / (1.0f - linear_zero_pos);
						else
							a = clicked_t;
						a = powf(a, power);
						new_value = ImLerp(ImMax(v_min, 0.0f), v_max, a);
					}
				}
				else
				{
					// Linear slider
					new_value = ImLerp(v_min, v_max, clicked_t);
				}

				// Round past decimal precision
				new_value = RoundScalar(new_value, decimal_precision);
				if (*v != new_value)
				{
					*v = new_value;
					value_changed = true;
				}
			}
		}

		// Draw
		float grab_t = ObliqueSliderBehaviorCalcRatioFromValue(*v, v_min, v_max, power, linear_zero_pos);
		if (!is_horizontal)
			grab_t = 1.0f - grab_t;
		const float grab_pos = ImLerp(slider_usable_pos_min, slider_usable_pos_max, grab_t);
		ImRect grab_bb;
		if (is_horizontal)
			grab_bb = ImRect(ImVec2(grab_pos - grab_sz * 0.5f, frame_bb.Min.y + grab_padding), ImVec2(grab_pos + grab_sz * 0.5f, frame_bb.Max.y - grab_padding));
		else
			grab_bb = ImRect(ImVec2(frame_bb.Min.x + grab_padding, grab_pos - grab_sz * 0.5f), ImVec2(frame_bb.Max.x - grab_padding, grab_pos + grab_sz * 0.5f));

		ImRect grabMin(ImVec2(grab_bb.Min.x + 6, frame_bb.Min.y), ImVec2(grab_bb.Min.x, frame_bb.Max.y));
		ImRect grabMax(ImVec2(grab_bb.Max.x, frame_bb.Max.y), ImVec2(grab_bb.Max.x + 6, frame_bb.Min.y));
		window->DrawList->AddQuadFilled(bbMin.Min, bbMin.Max, grabMax.Min, grabMax.Max, GetColorU32(g.ActiveId == id ? ImGuiCol_Border : ImGuiCol_Border));
		//window->DrawList->AddQuad(bbMin.Min, bbMin.Max, bbMax.Min, bbMax.Max, GetColorU32(ImVec4(1,1,1,1)));
		window->DrawList->AddQuad(bbMin.Min, bbMin.Max, bbMax.Min, bbMax.Max, GetColorU32(ImGuiCol_Border));

		//window->DrawList->AddRectFilled(grab_bb.Min, grab_bb.Max, GetColorU32(g.ActiveId == id ? ImGuiCol_SliderGrabActive : ImGuiCol_SliderGrab), style.GrabRounding);

		return value_changed;
	}

	// Use power!=1.0 for logarithmic sliders.
	// Adjust display_format to decorate the value with a prefix or a suffix.
	//   "%.3f"         1.234
	//   "%5.2f secs"   01.23 secs
	//   "Gold: %.0f"   Gold: 1
	IMGUI_API bool ObliqueSliderFloat(const char* label, float* v, float v_min, float v_max, const char* display_format = xorstr("%.3f"), float power = 1.0f)
	{
		ImGuiWindow* window = GetCurrentWindow();
		if (window->SkipItems)
			return false;

		ImGuiContext& g = *GImGui;
		const ImGuiStyle& style = g.Style;
		const ImGuiID id = window->GetID(label);
		const float w = CalcItemWidth();

		const ImVec2 label_size = CalcTextSize(label, NULL, true);
		const ImRect frame_bb(window->DC.CursorPos, window->DC.CursorPos + ImVec2(w, label_size.y + style.FramePadding.y * 2.0f));
		const ImRect total_bb(frame_bb.Min, frame_bb.Max + ImVec2(label_size.x > 0.0f ? style.ItemInnerSpacing.x + label_size.x : 0.0f, 0.0f));

		// NB- we don't call ItemSize() yet because we may turn into a text edit box below
		if (!ItemAdd(total_bb, id))
		{
			ItemSize(total_bb, style.FramePadding.y);
			return false;
		}
		const bool hovered = ItemHoverable(frame_bb, id);

		if (!display_format)
			display_format = xorstr("%.3f");
		int decimal_precision = ParseFormatPrecision(display_format, 3);

		// Tabbing or CTRL-clicking on Slider turns it into an input box
		bool start_text_input = false;
		const bool tab_focus_requested = FocusableItemRegister(window, id);
		if (tab_focus_requested || (hovered && g.IO.MouseClicked[0]))
		{
			SetActiveID(id, window);
			FocusWindow(window);
			if (tab_focus_requested || g.IO.KeyCtrl)
			{
				start_text_input = true;
				g.ScalarAsInputTextId = 0;
			}
		}
		if (start_text_input || (g.ActiveId == id && g.ScalarAsInputTextId == id))
			return InputScalarAsWidgetReplacement(frame_bb, label, ImGuiDataType_Float, v, id, decimal_precision);

		// Actual slider behavior + render grab
		ItemSize(total_bb, style.FramePadding.y);
		const bool value_changed = ObliqueSliderBehavior(frame_bb, id, v, v_min, v_max, power, decimal_precision);

		// Display value using user-provided display format so user can add prefix/suffix/decorations to the value.
		char value_buf[64];
		const char* value_buf_end = value_buf + ImFormatString(value_buf, IM_ARRAYSIZE(value_buf), display_format, *v);
		RenderTextClipped(frame_bb.Min, frame_bb.Max, value_buf, value_buf_end, NULL, ImVec2(0.5f, 0.5f));

		if (label_size.x > 0.0f)
			RenderText(ImVec2(frame_bb.Max.x + style.ItemInnerSpacing.x, frame_bb.Min.y + style.FramePadding.y), label);

		return value_changed;
	}
	IMGUI_API bool TabButton(const char* label, const ImVec2& size_arg, unsigned int index, ImGuiButtonFlags flags = 0)
	{
		ImGuiWindow* window = GetCurrentWindow();
		if (window->SkipItems)
			return false;

		ImGuiContext& g = *GImGui;
		const ImGuiStyle& style = g.Style;
		const ImGuiID id = window->GetID(label);
		const ImVec2 label_size = CalcTextSize(label, NULL, true);

		ImVec2 pos = window->DC.CursorPos;
		if ((flags & ImGuiButtonFlags_AlignTextBaseLine) && style.FramePadding.y < window->DC.CurrentLineTextBaseOffset) // Try to vertically align buttons that are smaller/have no padding so that text baseline matches (bit hacky, since it shouldn't be a flag)
			pos.y += window->DC.CurrentLineTextBaseOffset - style.FramePadding.y;
		ImVec2 size = CalcItemSize(size_arg, label_size.x + style.FramePadding.x * 2.0f, label_size.y + style.FramePadding.y * 2.0f);

		const ImRect bb(pos, pos + size);
		ItemSize(bb, style.FramePadding.y);
		if (!ItemAdd(bb, id))
			return false;

		if (window->DC.ItemFlags & ImGuiItemFlags_ButtonRepeat)
			flags |= ImGuiButtonFlags_Repeat;
		bool hovered, held;
		bool pressed = ButtonBehavior(bb, id, &hovered, &held, flags);

		const ImU32 col = GetColorU32((hovered && held) ? ImGuiCol_TabButtonActive : hovered ? ImGuiCol_TabButtonHovered : ImGuiCol_TabButton);
		RenderFrame(bb.Min, bb.Max, col, false, style.FrameRounding);
		const ImU32 border_col = GetColorU32((hovered && held) ? GetColorU32(ImVec4(1, 1, 1, 0.7)) : hovered ? GetColorU32(ImVec4(/*Global::RGB.x, Global::RGB.y, Global::RGB.z,*/ 1.0f, 1.0f, 1.0f, 0.45)) : GetColorU32(ImVec4(1, 1, 1, 0.7)));

		window->DrawList->AddRectFilledMultiColor(bb.Min, bb.Max, col, col, GetColorU32(ImVec4(30 / 255, 30 / 255, 30 / 255, 1)), GetColorU32(ImVec4(30 / 255, 30 / 255, 30 / 255, 1)));
		window->DrawList->AddRect(bb.Min, bb.Max, border_col, style.FrameRounding, ImDrawCornerFlags_All, 2.5f);
		RenderTextClipped(bb.Min + style.FramePadding, bb.Max - style.FramePadding, label, NULL, &label_size, style.ButtonTextAlign, &bb);
		if (!tab_info_already_exist(tabs_info, index))
		{
			TabInfo tab_info;
			tab_info.bb = bb;
			tab_info.index = index;
			tabs_info.push_back(tab_info);
		}

		return pressed;
	}
	IMGUI_API bool Tab(unsigned int index, const char* label, int* selected, float width = 0)
	{
		ImGuiStyle& style = ImGui::GetStyle();
		ImVec4 color = style.Colors[ImGuiCol_TabButton];
		ImVec4 colorActive = style.Colors[ImGuiCol_TabButtonActive];
		ImVec4 colorHover = style.Colors[ImGuiCol_TabButtonHovered];

		if (index > 0)
			ImGui::SameLine();

		if (index == *selected)
		{
			style.Colors[ImGuiCol_TabButton] = colorActive;
			style.Colors[ImGuiCol_TabButtonActive] = colorActive;
			style.Colors[ImGuiCol_TabButtonHovered] = colorActive;
		}
		else
		{
			style.Colors[ImGuiCol_TabButton] = color;
			style.Colors[ImGuiCol_TabButtonActive] = colorActive;
			style.Colors[ImGuiCol_TabButtonHovered] = colorHover;
		}

		if (TabButton(label, ImVec2(width, 30), index))
		{
			border_bang = 0.0f;
			old_tab_index = *selected;
			*selected = index;
		}

		style.Colors[ImGuiCol_TabButton] = color;
		style.Colors[ImGuiCol_TabButtonActive] = colorActive;
		style.Colors[ImGuiCol_TabButtonHovered] = colorHover;

		return *selected == index;
	}
	void TabBorderAnim(unsigned int current_tab, unsigned int old_tab)
	{
		if (tabs_info.size() > 0)
		{
			ImGuiWindow* window = GetCurrentWindow();
			if (window->SkipItems)
				return;
			auto& style = ImGui::GetStyle();
			auto old_tab_rect = tabs_info[old_tab].bb;
			auto tab_rect = tabs_info[current_tab].bb;
			auto tab_min = old_tab_rect.Min + (tab_rect.Min - old_tab_rect.Min) * border_bang;
			auto tab_max = old_tab_rect.Max + (tab_rect.Max - old_tab_rect.Max) * border_bang;
			window->DrawList->AddRect(tab_min, tab_max, ImGui::GetColorU32({ Global::RGB }), style.FrameRounding, ImDrawCornerFlags_All, 2.5f);
		}
	}
	IMGUI_API void SeparatorColored(ImVec4 color)
	{
		ImGuiWindow* window = GetCurrentWindow();

		if (window->SkipItems)
			return;
		ImGuiContext& g = *GImGui;

		ImGuiWindowFlags flags = 0;
		if ((flags & (ImGuiSeparatorFlags_Horizontal | ImGuiSeparatorFlags_Vertical)) == 0)
			flags |= (window->DC.LayoutType == ImGuiLayoutType_Horizontal) ? ImGuiSeparatorFlags_Vertical : ImGuiSeparatorFlags_Horizontal;
		IM_ASSERT(ImIsPowerOfTwo((int)(flags & (ImGuiSeparatorFlags_Horizontal | ImGuiSeparatorFlags_Vertical))));   // Check that only 1 option is selected
		if (flags & ImGuiSeparatorFlags_Vertical)
		{
			VerticalSeparator();
			return;
		}

		// Horizontal Separator
		if (window->DC.ColumnsSet)
			PopClipRect();

		float x1 = window->Pos.x;
		float x2 = window->Pos.x + window->Size.x;
		if (!window->DC.GroupStack.empty())
			x1 += window->DC.IndentX;

		const ImRect bb(ImVec2(x1, window->DC.CursorPos.y), ImVec2(x2, window->DC.CursorPos.y + 2.5f));
		ItemSize(ImVec2(0.0f, 0.0f)); // NB: we don't provide our width so that it doesn't get feed back into AutoFit, we don't provide height to not alter layout.
		if (!ItemAdd(bb, 0))
		{
			if (window->DC.ColumnsSet)
				PushColumnClipRect();
			return;
		}

		window->DrawList->AddLine(bb.Min, ImVec2(bb.Max.x, bb.Min.y), GetColorU32(color));
		GetOverlayDrawList()->AddLine(bb.Min, ImVec2(bb.Max.x, bb.Min.y), GetColorU32(color));

		if (window->DC.ColumnsSet)
		{
			PushColumnClipRect();
			window->DC.ColumnsSet->CellMinY = window->DC.CursorPos.y;
		}
	}
}

VOID AddMarker(ImGuiWindow& window, float width, float height, float* start, PVOID pawn, LPCSTR text, ImU32 color) {
	float minX = FLT_MAX;
	float maxX = -FLT_MAX;
	float minY = FLT_MAX;
	float maxY = -FLT_MAX;
	if (minX < width && maxX > 0 && minY < height && maxY > 0) {
		auto topLeft = ImVec2(minX - 3.0f, minY - 3.0f);
		auto bottomRight = ImVec2(maxX + 3.0f, maxY + 3.0f);
		auto centerTop = ImVec2((topLeft.x + bottomRight.x) / 2.0f, topLeft.y);
		auto root = Util::GetPawnRootLocation(pawn);
		if (root) {
			auto pos = *root;
			float dx = start[0] - pos.X;
			float dy = start[1] - pos.Y;
			float dz = start[2] - pos.Z;

			if (Util::WorldToScreen(width, height, &pos.X)) {
				float dist = Util::SpoofCall(sqrtf, dx * dx + dy * dy + dz * dz) / 1000.0f;

				CHAR modified[0xFF] = { 0 };
				snprintf(modified, sizeof(modified), ("%s - %dm"), text, static_cast<INT>(dist));

				auto size = ImGui::GetFont()->CalcTextSizeA(window.DrawList->_Data->FontSize, FLT_MAX, 0, modified);
				window.DrawList->AddRectFilled(ImVec2(centerTop.x - size.x / 2.0f, centerTop.y - size.y + 3.0f), ImVec2(centerTop.x + size.x / 2.0f, centerTop.y), ImGui::GetColorU32({ 0.0f, 0.0f, 0.0f, 0.4f }));
				window.DrawList->AddText(ImVec2(pos.X - size.x / 2.0f, pos.Y - size.y / 2.0f), color, modified);
			}
		}
	}
}
__declspec(dllexport) LRESULT CALLBACK WndProcHook(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	if (msg == WM_KEYUP && (wParam == VK_INSERT || (ShowMenu && wParam == VK_ESCAPE))) {
		ShowMenu = !ShowMenu;
		ImGui::GetIO().MouseDrawCursor = ShowMenu;
	}
	else if (msg == WM_QUIT && ShowMenu) {
		ExitProcess(0);
	}

	if (ShowMenu) {
		ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);
		return TRUE;
	}

	return CallWindowProc(oWndProc, hWnd, msg, wParam, lParam);
}

extern uint64_t base_address = 0;
DWORD processID;
const ImVec4 color = { 255.0,255.0,255.0,1 };
const ImVec4 red = { 0.65,0,0,1 };
const ImVec4 white = { 255.0,255.0,255.0,1 };
const ImVec4 green = { 0.03,0.81,0.14,1 };
const ImVec4 blue = { 0.21960784313,0.56470588235,0.90980392156,1.0 };

ImGuiWindow& BeginScene() {
	ImGui_ImplDX11_NewFrame();
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
	ImGui::Begin(("##scene"), nullptr, ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoTitleBar);

	auto& io = ImGui::GetIO();
	ImGui::SetWindowPos(ImVec2(0, 0), ImGuiCond_Always);
	ImGui::SetWindowSize(ImVec2(io.DisplaySize.x, io.DisplaySize.y), ImGuiCond_Always);

	return *ImGui::GetCurrentWindow();
}

VOID EndScene(ImGuiWindow& window) {
	window.DrawList->PushClipRectFullScreen();
	//ImGui::End();
	ImGui::PopStyleColor();
	ImGui::PopStyleVar(2);
	ImGui::Render();
}
VOID AddLine(ImGuiWindow& window, float width, float height, float a[3], float b[3], ImU32 color, float& minX, float& maxX, float& minY, float& maxY) {
	float ac[3] = { a[0], a[1], a[2] };
	float bc[3] = { b[0], b[1], b[2] };
	if (Util::WorldToScreen(width, height, ac) && Util::WorldToScreen(width, height, bc)) {
		window.DrawList->AddLine(ImVec2(ac[0], ac[1]), ImVec2(bc[0], bc[1]), color, 2.0f);

		minX = min(ac[0], minX);
		minX = min(bc[0], minX);

		maxX = max(ac[0], maxX);
		maxX = max(bc[0], maxX);

		minY = min(ac[1], minY);
		minY = min(bc[1], minY);

		maxY = max(ac[1], maxY);
		maxY = max(bc[1], maxY);
	}
}
__declspec(dllexport) HRESULT PresentHook(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
	static float width = 0;
	static float height = 0;
	static HWND hWnd = 0;
	if (!device) {
		swapChain->GetDevice(__uuidof(device), reinterpret_cast<PVOID*>(&device));
		device->GetImmediateContext(&immediateContext);

		ID3D11Texture2D* renderTarget = nullptr;
		swapChain->GetBuffer(0, __uuidof(renderTarget), reinterpret_cast<PVOID*>(&renderTarget));
		device->CreateRenderTargetView(renderTarget, nullptr, &renderTargetView);
		renderTarget->Release();

		ID3D11Texture2D* backBuffer = 0;
		swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (PVOID*)&backBuffer);
		D3D11_TEXTURE2D_DESC backBufferDesc = { 0 };
		backBuffer->GetDesc(&backBufferDesc);

		hWnd = FindWindow((L"UnrealWindow"), (L"Fortnite  "));
		if (!width) {
			oWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtr(hWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WndProcHook)));
		}

		width = (float)backBufferDesc.Width;
		height = (float)backBufferDesc.Height;
		backBuffer->Release();

		ImGui::GetIO().Fonts->AddFontFromFileTTF(("C:\\Windows\\Fonts\\arialbd.ttf"), 12.0f);

		ImGui_ImplDX11_Init(hWnd, device, immediateContext);
		ImGui_ImplDX11_CreateDeviceObjects();
	}
	immediateContext->OMSetRenderTargets(1, &renderTargetView, nullptr);
	////// reading
	auto& window = BeginScene();
	////// reading
	if (ShowMenu)
	{
		ImVec2 cursor_pos = { 0,0 };
		enum tab_id : int
		{
			Aimbot,
			Visuals,
			Misc,
			Info
		};
		static int tab_index = 3;
		static int tab_index = 3;
		void draw_visuals_tab()
		{
			//ImGui::Columns(1, NULL, false);
			//ImGui::BeginChild(xorstr("player_esp_child"), ImVec2(450, 350), true);
			ImGui::SeparatorColored(ImGui::GetStyleColorVec4(ImGuiCol_Separator));
			ImGui::SetCursorPosX(ImGui::GetWindowWidth() - (ImGui::CalcTextSize(xorstr("player visuals"), NULL, TRUE).x + ImGui::GetWindowWidth()) / 2);
			ImGui::Text(xorstr("player visuals"));
			ImGui::SeparatorColored(ImGui::GetStyleColorVec4(ImGuiCol_Separator));
			ImGui::Checkbox(xorstr("Covid ESP"), &Settings.ESP.PlayerAmmo);
			ImGui::Checkbox(xorstr("Covid Crosshair"), &Settings.Crosshair);
			ImGui::Checkbox(xorstr("Covid Boat ESP"), &Settings.ESP.Boxes);
			ImGui::Checkbox(xorstr("Covid Ammo ESP"), &Settings.ESP.debug);
			ImGui::Checkbox(xorstr("Covid Helicopter ESP"), &Settings.ESP.PlayerNames);
			ImGui::Checkbox(xorstr("Covid Car ESP"), &Settings.ESP.Radar);
			ImGui::Spacing();
			//ImGui::EndChild();
		}
		void draw_aimbot_tab()
		{
			//ImGui::Columns(1, NULL, false);
			//ImGui::BeginChild(xorstr("##aimbot_main_child"), ImVec2(450, 350), true);
			ImGui::SeparatorColored(ImGui::GetStyleColorVec4(ImGuiCol_Separator));
			ImGui::SetCursorPosX(ImGui::GetWindowWidth() - (ImGui::CalcTextSize(xorstr("aimbot settings"), NULL, TRUE).x + ImGui::GetWindowWidth()) / 2);
			ImGui::Text(xorstr("Aimbot"));
			ImGui::SeparatorColored(ImGui::GetStyleColorVec4(ImGuiCol_Separator));
			ImGui::Checkbox(xorstr("Aimbot"), &Settings.Aimbot);
			ImGui::Checkbox(xorstr("Silent Aimbot"), &Settings.SilentAimbot);
			ImGui::SeparatorColored(ImGui::GetStyleColorVec4(ImGuiCol_Separator));
			ImGui::SetCursorPosX(ImGui::GetWindowWidth() - (ImGui::CalcTextSize(xorstr("aimbot adjust"), NULL, TRUE).x + ImGui::GetWindowWidth()) / 2);
			ImGui::Text(xorstr("Aimbot Settings"));
			ImGui::SeparatorColored(ImGui::GetStyleColorVec4(ImGuiCol_Separator));
			ImGui::Checkbox(xorstr("Covid FOV Circle"), &Settings.ColorAdjuster);
			ImGui::SliderFloat(xorstr("##FOV"), &Settings.AimbotFOV, 0, 1000, xorstr("FOV Circle: %.2f"));
			ImGui::SliderFloat(xorstr("##AimSmooth"), &Settings.smooth1, 0, 5, xorstr("Aim Smooth: %.2f"));
			//ImGui::EndChild();
		}
		void draw_magic_tab()
		{
			//ImGui::Columns(1, NULL, false);
			//ImGui::BeginChild(xorstr("##misc_main_child"), ImVec2(450, 350), true);
			ImGui::SeparatorColored(ImGui::GetStyleColorVec4(ImGuiCol_Separator));
			ImGui::SetCursorPosX(ImGui::GetWindowWidth() - (ImGui::CalcTextSize(xorstr("misc settings"), NULL, TRUE).x + ImGui::GetWindowWidth()) / 2);
			ImGui::Text(xorstr("Misc"));
			ImGui::SeparatorColored(ImGui::GetStyleColorVec4(ImGuiCol_Separator));
			ImGui::Button(("Invisible"));
			ImGui::SameLine();
			ImGui::Button(("Visible"));
			ImGui::Checkbox(xorstr("Spinbot"), &Settings.debug1);
			ImGui::Checkbox(xorstr("Airstuck"), &Settings.debug2);
			ImGui::Checkbox(xorstr("Instant Reload"), &Settings.debug3);
			ImGui::Checkbox(xorstr("Fast Actions"), &Settings.debug4);
			ImGui::Checkbox(xorstr("Rapid Fire"), &Settings.debug5);
			ImGui::Checkbox(xorstr("Vehicle Speed [only enable in Vehicle]"), &Settings.debug6);
			//ImGui::EndChild();
		}
		void draw_info_tab()
		{
			//ImGui::Columns(1, NULL, false);
			//ImGui::BeginChild(xorstr("##misc_main_child"), ImVec2(450, 350), true);
			ImGui::SeparatorColored(ImGui::GetStyleColorVec4(ImGuiCol_Separator));
			ImGui::SetCursorPosX(ImGui::GetWindowWidth() - (ImGui::CalcTextSize(xorstr("misc settings"), NULL, TRUE).x + ImGui::GetWindowWidth()) / 2);
			ImGui::Text(xorstr("Information"));
			ImGui::SeparatorColored(ImGui::GetStyleColorVec4(ImGuiCol_Separator));
			ImGui::Text(xorstr("                            Thanks for using this Cheat!"));
			ImGui::Text(xorstr(""));
			ImGui::Text(xorstr("                                           Credits"));
			ImGui::Text(xorstr(""));
			ImGui::Text(xorstr("                              Menu         :   YTMcGamer#1337"));
			ImGui::Text(xorstr("                              Exploits    :   Kenny's Cheetos#7872"));
			ImGui::Text(xorstr("                              Discord    :   discord.gg/rEJKs8"));

			//ImGui::EndChild();
		}
		/// 0 = FLAT APPEARENCE
		/// 1 = MORE "3D" LOOK
		int is3D = 0;

		ImVec4* colors = ImGui::GetStyle().Colors;
		colors[ImGuiCol_Text] = ImVec4(1.00f, 1.00f, 1.00f, 1.f);
		colors[ImGuiCol_TextDisabled] = ImVec4(1.00f, 0.90f, 0.19f, 1.f);
		colors[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.06f, 0.06f, 1.f);
		colors[ImGuiCol_ChildBg] = ImVec4(0.08f, 0.08f, 0.08f, 1.f);
		colors[ImGuiCol_PopupBg] = ImVec4(0.08f, 0.08f, 0.08f, 0.94f);
		colors[ImGuiCol_Border] = ImVec4(0.08f, 0.08f, 0.08f, 1.f);
		colors[ImGuiCol_BorderShadow] = ImVec4(0.08f, 0.08f, 0.08f, 1.f);
		colors[ImGuiCol_Separator] = ImVec4(0.08f, 0.08f, 0.08f, 1.f);
		colors[ImGuiCol_FrameBg] = ImVec4(0.10f, 0.10f, 0.10f, 1.f);
		colors[ImGuiCol_FrameBgHovered] = ImVec4(0.21f, 0.21f, 0.21f, 0.78f);
		colors[ImGuiCol_FrameBgActive] = ImVec4(0.28f, 0.27f, 0.27f, 1.f);
		colors[ImGuiCol_TitleBg] = ImVec4(0.06f, 0.06f, 0.06f, 1.f);
		colors[ImGuiCol_TitleBgActive] = ImVec4(0.06f, 0.06f, 0.06f, 1.f);
		colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.06f, 0.06f, 0.06f, 1.f);
		colors[ImGuiCol_MenuBarBg] = ImVec4(0.14f, 0.14f, 0.14f, 1.f);
		colors[ImGuiCol_ScrollbarBg] = ImVec4(0.15f, 0.15f, 0.15f, 0.8f);
		colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.19f, 0.19f, 0.19f, 1.f);
		colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.19f, 0.19f, 0.19f, 1.f);
		colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.25f, 0.25f, 0.25f, 1.f);
		colors[ImGuiCol_CheckMark] = ImVec4(1.0f, 1.0f, 1.0f, 1.f);
		colors[ImGuiCol_SliderGrab] = ImVec4(1.0f, 1.0f, 1.0f, 1.f);
		colors[ImGuiCol_SliderGrabActive] = ImVec4(1.0f, 1.0f, 1.0f, 1.f);
		colors[ImGuiCol_Button] = ImVec4(0.20f, 0.20f, 0.20f, 1.f);
		colors[ImGuiCol_ButtonHovered] = ImVec4(0.25f, 0.25f, 0.25f, 1.f);
		colors[ImGuiCol_ButtonActive] = ImVec4(0.45f, 0.45f, 0.45f, 1.f);;
		colors[ImGuiCol_Header] = ImVec4(0.19f, 0.19f, 0.19f, 1.f);
		colors[ImGuiCol_HeaderHovered] = ImVec4(0.19f, 0.19f, 0.19f, 1.f);
		colors[ImGuiCol_HeaderActive] = ImVec4(0.25f, 0.25f, 0.25f, 1.f);
		colors[ImGuiCol_Separator] = ImVec4(0.38f, 0.38f, 0.38f, 0.5f);
		colors[ImGuiCol_SeparatorHovered] = ImVec4(0.46f, 0.46f, 0.46f, 0.5f);
		colors[ImGuiCol_SeparatorActive] = ImVec4(0.46f, 0.46f, 0.46f, 0.64f);
		colors[ImGuiCol_ResizeGrip] = ImVec4(0.26f, 0.26f, 0.26f, 1.f);
		colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.31f, 0.31f, 0.31f, 1.f);
		colors[ImGuiCol_ResizeGripActive] = ImVec4(0.35f, 0.35f, 0.35f, 1.f);
		colors[ImGuiCol_PlotLines] = ImVec4(0.61f, 0.61f, 0.61f, 1.f);
		colors[ImGuiCol_PlotLinesHovered] = ImVec4(1.00f, 0.43f, 0.35f, 1.f);
		colors[ImGuiCol_PlotHistogram] = ImVec4(0.90f, 0.70f, 0.00f, 1.f);
		colors[ImGuiCol_PlotHistogramHovered] = ImVec4(1.00f, 0.60f, 0.00f, 1.f);
		colors[ImGuiCol_TextSelectedBg] = ImVec4(0.26f, 0.59f, 0.98f, 0.35f);
		colors[ImGuiCol_DragDropTarget] = ImVec4(1.00f, 1.00f, 0.00f, 0.9f);

		auto& style = ImGui::GetStyle();
		ImGui::GetStyle().WindowPadding = { 10.f , 10.f };
		ImGui::GetStyle().PopupRounding = 0.f;
		ImGui::GetStyle().FramePadding = { 0.f, 0.f };
		ImGui::GetStyle().ItemSpacing = { 10.f, 8.f };
		ImGui::GetStyle().ItemInnerSpacing = { 6.f, 6.f };
		ImGui::GetStyle().TouchExtraPadding = { 0.f, 0.f };
		ImGui::GetStyle().IndentSpacing = 21.f;
		ImGui::GetStyle().ScrollbarSize = 15.f;
		ImGui::GetStyle().GrabMinSize = 8.f;
		ImGui::GetStyle().WindowTitleAlign = ImVec2(0.5f, .0f);
		ImGui::GetStyle().WindowBorderSize = 1.5f;
		ImGui::GetStyle().ChildBorderSize = 1.5f;
		ImGui::GetStyle().PopupBorderSize = 1.5f;
		ImGui::GetStyle().FrameBorderSize = 0.f;
		ImGui::GetStyle().WindowRounding = 3.f;
		ImGui::GetStyle().ChildRounding = 3.f;
		ImGui::GetStyle().FrameRounding = 1.0f;
		ImGui::GetStyle().ScrollbarRounding = 1.f;
		ImGui::GetStyle().GrabRounding = 1.f;
		ImGui::GetStyle().ButtonTextAlign = { 0.5f, 0.5f };
		ImGui::GetStyle().DisplaySafeAreaPadding = { 3.f, 3.f };

		ImGui::PushStyleColor(ImGuiCol_Border, Global::RGB);

		if (ImGui::Begin(std::string(std::string(xorstr("OpensrcFN [RELEASE] "))).c_str(), 0, ImGuiWindowFlags_::ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_::ImGuiWindowFlags_NoResize))
		{
			ImGui::BeginChild(xorstr("##main"), ImVec2(450, 400), true);
			ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.19f, 0.19f, 0.19f, 1.f));
			cursor_pos = ImGui::GetCursorPos();
			ImGui::Image((void*)Global::my_texture, ImVec2(Global::my_image_width, Global::my_image_height), ImVec2(0, 0), ImVec2(1, 1), Global::RGB); ImGui::SameLine();
			ImGui::SetCursorPosX(ImGui::GetWindowWidth() - (ImGui::GetWindowWidth() - Global::my_image_width + 300) / 2);
			ImGui::Tab(0, (xorstr("Aimbot")), &tab_index, 100); ImGui::SameLine();
			ImGui::Tab(1, (xorstr("Visuals")), &tab_index, 100); ImGui::SameLine();
			ImGui::Tab(2, (xorstr("Magic")), &tab_index, 100);
			ImGui::SetCursorPosX(ImGui::GetWindowWidth() - (ImGui::GetWindowWidth() - Global::my_image_width + 300) / 2);
			ImGui::Text(xorstr("Made by YTMcGamer#1337"));
			if (border_bang < 1.0f)
				border_bang = clip(border_bang + 1.0f / 0.15f * ImGui::GetIO().DeltaTime, 0.0f, 1.0f);

			ImGui::TabBorderAnim(tab_index, old_tab_index);
			tabs_info.clear();
			switch (tab_index)
			{
			case tab_id::Aimbot:
				draw_aimbot_tab();
				break;
			case tab_id::Visuals:
				draw_visuals_tab();
				break;
			case tab_id::Misc:
				draw_magic_tab();
				break;
			case tab_id::Info:
				draw_info_tab();
				break;
			}
			ImGui::PopStyleColor();
			ImGui::EndChild();
		}ImGui::End();
		ImGui::PopStyleColor();
	}

	auto success = FALSE;
	do {
		float closestDistance = FLT_MAX;
		PVOID closestPawn = NULL;

		auto world = *Offsets::uWorld;
		if (!world) break;

		auto gameInstance = ReadPointer(world, Offsets::Engine::World::OwningGameInstance);
		if (!gameInstance) break;

		auto localPlayers = ReadPointer(gameInstance, Offsets::Engine::GameInstance::LocalPlayers);
		if (!localPlayers) break;

		auto localPlayer = ReadPointer(localPlayers, 0);
		if (!localPlayer) break;

		auto localPlayerController = ReadPointer(localPlayer, Offsets::Engine::Player::PlayerController);
		if (!localPlayerController) break;

		auto localPlayerPawn = reinterpret_cast<UObject*>(ReadPointer(localPlayerController, Offsets::Engine::PlayerController::AcknowledgedPawn));
		if (!localPlayerPawn) break;

		auto localPlayerWeapon = ReadPointer(localPlayerPawn, Offsets::FortniteGame::FortPawn::CurrentWeapon);
		if (!localPlayerWeapon) break;

		auto localPlayerRoot = ReadPointer(localPlayerPawn, Offsets::Engine::Actor::RootComponent);
		if (!localPlayerRoot) break;

		auto localPlayerState = ReadPointer(localPlayerPawn, Offsets::Engine::Pawn::PlayerState);
		if (!localPlayerState) break;

		auto localPlayerLocation = reinterpret_cast<float*>(reinterpret_cast<PBYTE>(localPlayerRoot) + Offsets::Engine::SceneComponent::RelativeLocation);
		auto localPlayerTeamIndex = ReadDWORD(localPlayerState, Offsets::FortniteGame::FortPlayerStateAthena::TeamIndex);

		auto weaponName = Util::GetObjectFirstName((UObject*)localPlayerWeapon);
		auto isProjectileWeapon = wcsstr(weaponName.c_str(), L"Rifle_Sniper");

		Core::LocalPlayerPawn = localPlayerPawn;
		Core::LocalPlayerController = localPlayerController;

		std::vector<PVOID> playerPawns;
		for (auto li = 0UL; li < ReadDWORD(world, Offsets::Engine::World::Levels + sizeof(PVOID)); ++li) {
			auto levels = ReadPointer(world, 0x148);//Levels
			if (!levels) break;

			auto level = ReadPointer(levels, li * sizeof(PVOID));
			if (!level) continue;

			for (auto ai = 0UL; ai < ReadDWORD(level, Offsets::Engine::Level::AActors + sizeof(PVOID)); ++ai) {
				auto actors = ReadPointer(level, Offsets::Engine::Level::AActors);
				if (!actors) break;

				auto pawn = reinterpret_cast<UObject*>(ReadPointer(actors, ai * sizeof(PVOID)));
				if (!pawn || pawn == localPlayerPawn) continue;

				auto name = Util::GetObjectFirstName(pawn);
				if (wcsstr(name.c_str(), L"PlayerPawn_Athena_C") || wcsstr(name.c_str(), L"PlayerPawn_Athena_Phoebe_C")) {
					playerPawns.push_back(pawn);
				}
				else if (wcsstr(name.c_str(), L"FortPickupAthena")) {
					auto item = ReadPointer(pawn, Offsets::FortniteGame::FortPickup::PrimaryPickupItemEntry + Offsets::FortniteGame::FortItemEntry::ItemDefinition);
					if (!item) continue;

					auto itemName = reinterpret_cast<FText*>(ReadPointer(item, Offsets::FortniteGame::FortItemDefinition::DisplayName));
					if (!itemName || !itemName->c_str()) continue;

					auto isAmmo = wcsstr(itemName->c_str(), L"Ammo: ");
					if ((!Settings.ESP.Ammo && isAmmo) || ((!Settings.ESP.Weapons || ReadBYTE(item, Offsets::FortniteGame::FortItemDefinition::Tier) < Settings.ESP.MinWeaponTier) && !isAmmo)) continue;

					CHAR text[0xFF] = { 0 };
					wcstombs(text, itemName->c_str() + (isAmmo ? 6 : 0), sizeof(text));

					AddMarker(window, width, height, localPlayerLocation, pawn, text, isAmmo ? ImGui::GetColorU32({ 0.75f, 0.75f, 0.75f, 1.0f }) : ImGui::GetColorU32({ 1.0f, 1.0f, 1.0f, 1.0f }));
				}
				else if (Settings.ESP.Containers && wcsstr(name.c_str(), L"Tiered_Chest") && !((ReadBYTE(pawn, Offsets::FortniteGame::BuildingContainer::bAlreadySearched) >> 7) & 1)) {
					AddMarker(window, width, height, localPlayerLocation, pawn, "Chest", ImGui::GetColorU32({ 1.0f, 0.84f, 0.0f, 1.0f }));
				}
				else if (Settings.ESP.Containers && wcsstr(name.c_str(), L"AthenaSupplyDrop_Llama")) {
					AddMarker(window, width, height, localPlayerLocation, pawn, "Llama", ImGui::GetColorU32({ 1.0f, 0.0f, 0.0f, 1.0f }));
				}
				else if (Settings.ESP.Ammo && wcsstr(name.c_str(), L"Tiered_Ammo") && !((ReadBYTE(pawn, Offsets::FortniteGame::BuildingContainer::bAlreadySearched) >> 7) & 1)) {
					AddMarker(window, width, height, localPlayerLocation, pawn, "Ammo Box", ImGui::GetColorU32({ 0.75f, 0.75f, 0.75f, 1.0f }));
				}
			}
		}
		printf("\nplayer pawns : %p.", playerPawns);

		for (auto pawn : playerPawns)
		{
			auto state = ReadPointer(pawn, 0x238);
			if (!state) continue;

			auto mesh = ReadPointer(pawn, 0x278);
			if (!mesh) continue;

			auto bones = ReadPointer(mesh, 0x420);
			if (!bones) bones = ReadPointer(mesh, 0x420 + 0x10);
			if (!bones) continue;

			float compMatrix[4][4] = { 0 };
			Util::ToMatrixWithScale(reinterpret_cast<float*>(reinterpret_cast<PBYTE>(mesh) + 0x1C0), compMatrix);

			// Top
			float head[3] = { 0 };
			Util::GetBoneLocation(compMatrix, bones, 66, head);

			float neck[3] = { 0 };
			Util::GetBoneLocation(compMatrix, bones, 65, neck);

			float chest[3] = { 0 };
			Util::GetBoneLocation(compMatrix, bones, 36, chest);

			float pelvis[3] = { 0 };
			Util::GetBoneLocation(compMatrix, bones, 2, pelvis);

			// Arms
			float leftShoulder[3] = { 0 };
			Util::GetBoneLocation(compMatrix, bones, 9, leftShoulder);

			float rightShoulder[3] = { 0 };
			Util::GetBoneLocation(compMatrix, bones, 62, rightShoulder);

			float leftElbow[3] = { 0 };
			Util::GetBoneLocation(compMatrix, bones, 10, leftElbow);

			float rightElbow[3] = { 0 };
			Util::GetBoneLocation(compMatrix, bones, 38, rightElbow);

			float leftHand[3] = { 0 };
			Util::GetBoneLocation(compMatrix, bones, 11, leftHand);

			float rightHand[3] = { 0 };
			Util::GetBoneLocation(compMatrix, bones, 39, rightHand);

			// Legs
			float leftLeg[3] = { 0 };
			Util::GetBoneLocation(compMatrix, bones, 67, leftLeg);

			float rightLeg[3] = { 0 };
			Util::GetBoneLocation(compMatrix, bones, 74, rightLeg);

			float leftThigh[3] = { 0 };
			Util::GetBoneLocation(compMatrix, bones, 73, leftThigh);

			float rightThigh[3] = { 0 };
			Util::GetBoneLocation(compMatrix, bones, 80, rightThigh);

			float leftFoot[3] = { 0 };
			Util::GetBoneLocation(compMatrix, bones, 68, leftFoot);

			float rightFoot[3] = { 0 };
			Util::GetBoneLocation(compMatrix, bones, 75, rightFoot);

			float leftFeet[3] = { 0 };
			Util::GetBoneLocation(compMatrix, bones, 71, leftFeet);

			float rightFeet[3] = { 0 };
			Util::GetBoneLocation(compMatrix, bones, 78, rightFeet);

			float leftFeetFinger[3] = { 0 };
			Util::GetBoneLocation(compMatrix, bones, 72, leftFeetFinger);

			float rightFeetFinger[3] = { 0 };
			Util::GetBoneLocation(compMatrix, bones, 79, rightFeetFinger);

			auto color = ImGui::GetColorU32({ Settings.ESP.PlayerNotVisibleColor[0], Settings.ESP.PlayerNotVisibleColor[1], Settings.ESP.PlayerNotVisibleColor[2], 1.0f });
			FVector viewPoint = { 0 };

			if (ReadDWORD(state, 0xE60) == localPlayerTeamIndex) {
				color = ImGui::GetColorU32({ 0.0f, 1.0f, 0.0f, 1.0f });
			}
			else if ((ReadBYTE(pawn, Offsets::FortniteGame::FortPawn::bIsDBNO) & 1) && (isProjectileWeapon || Util::LineOfSightTo(localPlayerController, pawn, &viewPoint))) {
				color = ImGui::GetColorU32({ Settings.ESP.PlayerVisibleColor[0], Settings.ESP.PlayerVisibleColor[1], Settings.ESP.PlayerVisibleColor[2], 1.0f });
				if (Settings.AutoAimbot) {
					auto dx = head[0] - localPlayerLocation[0];
					auto dy = head[1] - localPlayerLocation[1];
					auto dz = head[2] - localPlayerLocation[2];
					auto dist = dx * dx + dy * dy + dz * dz;
					if (dist < closestDistance) {
						closestDistance = dist;
						closestPawn = pawn;
					}
				}
				else
				{
					auto w2s = *reinterpret_cast<FVector*>(head);
					if (Util::WorldToScreen(width, height, &w2s.X)) {
						auto dx = w2s.X - (width / 2);
						auto dy = w2s.Y - (height / 2);
						auto dist = Util::SpoofCall(sqrtf, dx * dx + dy * dy);
						if (dist < Settings.AimbotFOV && dist < closestDistance) {
							closestDistance = dist;
							closestPawn = pawn;
						}
					}
				}
			}

			if (!Settings.ESP.Players) continue;

			if (Settings.ESP.PlayerLines) {
				auto end = *reinterpret_cast<FVector*>(head);
				if (Util::WorldToScreen(width, height, &end.X)) {
					window.DrawList->AddLine(ImVec2(width / 2, height), ImVec2(end.X, end.Y), color);
				}
			}

			float minX = FLT_MAX;
			float maxX = -FLT_MAX;
			float minY = FLT_MAX;
			float maxY = -FLT_MAX;

			AddLine(window, width, height, head, neck, color, minX, maxX, minY, maxY);
			AddLine(window, width, height, neck, pelvis, color, minX, maxX, minY, maxY);
			AddLine(window, width, height, chest, leftShoulder, color, minX, maxX, minY, maxY);
			AddLine(window, width, height, chest, rightShoulder, color, minX, maxX, minY, maxY);
			AddLine(window, width, height, leftShoulder, leftElbow, color, minX, maxX, minY, maxY);
			AddLine(window, width, height, rightShoulder, rightElbow, color, minX, maxX, minY, maxY);
			AddLine(window, width, height, leftElbow, leftHand, color, minX, maxX, minY, maxY);
			AddLine(window, width, height, rightElbow, rightHand, color, minX, maxX, minY, maxY);
			AddLine(window, width, height, pelvis, leftLeg, color, minX, maxX, minY, maxY);
			AddLine(window, width, height, pelvis, rightLeg, color, minX, maxX, minY, maxY);
			AddLine(window, width, height, leftLeg, leftThigh, color, minX, maxX, minY, maxY);
			AddLine(window, width, height, rightLeg, rightThigh, color, minX, maxX, minY, maxY);
			AddLine(window, width, height, leftThigh, leftFoot, color, minX, maxX, minY, maxY);
			AddLine(window, width, height, rightThigh, rightFoot, color, minX, maxX, minY, maxY);
			AddLine(window, width, height, leftFoot, leftFeet, color, minX, maxX, minY, maxY);
			AddLine(window, width, height, rightFoot, rightFeet, color, minX, maxX, minY, maxY);
			AddLine(window, width, height, leftFeet, leftFeetFinger, color, minX, maxX, minY, maxY);
			AddLine(window, width, height, rightFeet, rightFeetFinger, color, minX, maxX, minY, maxY);

			if (minX < width && maxX > 0 && minY < height && maxY > 0) {
				auto topLeft = ImVec2(minX - 3.0f, minY - 3.0f);
				auto bottomRight = ImVec2(maxX + 3.0f, maxY + 3.0f);

				window.DrawList->AddRectFilled(topLeft, bottomRight, ImGui::GetColorU32({ 0.0f, 0.0f, 0.0f, 0.20f }));
				window.DrawList->AddRect(topLeft, bottomRight, ImGui::GetColorU32({ 0.0f, 0.50f, 0.90f, 1.0f }), 0.5, 15, 1.5f);

				if (Settings.ESP.PlayerNames) {
					FString playerName;
					Core::ProcessEvent(state, Offsets::Engine::PlayerState::GetPlayerName, &playerName, 0);
					if (playerName.c_str()) {
						CHAR copy[0xFF] = { 0 };
						wcstombs(copy, playerName.c_str(), sizeof(copy));
						Util::FreeInternal(playerName.c_str());

						auto centerTop = ImVec2((topLeft.x + bottomRight.x) / 2.0f, topLeft.y);
						auto size = ImGui::GetFont()->CalcTextSizeA(window.DrawList->_Data->FontSize, FLT_MAX, 0, copy);
						window.DrawList->AddRectFilled(ImVec2(centerTop.x - size.x / 2.0f, centerTop.y - size.y + 3.0f), ImVec2(centerTop.x + size.x / 2.0f, centerTop.y), ImGui::GetColorU32({ 0.0f, 0.0f, 0.0f, 0.4f }));
						window.DrawList->AddText(ImVec2(centerTop.x - size.x / 2.0f, centerTop.y - size.y), color, copy);
					}
				}
			}
		}

		if (Settings.Aimbot && closestPawn && Util::SpoofCall(GetAsyncKeyState, VK_RBUTTON) < 0 && Util::SpoofCall(GetForegroundWindow) == hWnd) {
			Core::TargetPawn = closestPawn;
			Core::NoSpread = FALSE;
			//printf("\nworked?");
		}
		else {
			Core::TargetPawn = nullptr;
			Core::NoSpread = FALSE;
		}
		if (!Settings.AutoAimbot && Settings.ESP.AimbotFOV) {
			window.DrawList->AddCircle(ImVec2(width / 2, height / 2), Settings.AimbotFOV, ImGui::GetColorU32({ 255.0,255.0,255.0, 0.20f }), 105);
			window.DrawList->AddCircle(ImVec2(width / 2, height / 2), Settings.AimbotFOV + 1, ImGui::GetColorU32({ 255.0,255.0,255.0, 0.20f }), 105);
			window.DrawList->AddCircle(ImVec2(width / 2, height / 2), Settings.AimbotFOV + 2, ImGui::GetColorU32({ 255.0,255.0,255.0, 0.18f }), 105);
			window.DrawList->AddCircle(ImVec2(width / 2, height / 2), Settings.AimbotFOV + 3, ImGui::GetColorU32({ 255.0,255.0,255.0, 0.18f }), 105);
			window.DrawList->AddCircle(ImVec2(width / 2, height / 2), Settings.AimbotFOV + 4, ImGui::GetColorU32({ 255.0,255.0,255.0, 0.16f }), 105);
			window.DrawList->AddCircle(ImVec2(width / 2, height / 2), Settings.AimbotFOV + 5, ImGui::GetColorU32({ 255.0,255.0,255.0, 0.16f }), 105);
			window.DrawList->AddCircle(ImVec2(width / 2, height / 2), Settings.AimbotFOV + 6, ImGui::GetColorU32({ 255.0,255.0,255.0, 0.14f }), 105);
			window.DrawList->AddCircle(ImVec2(width / 2, height / 2), Settings.AimbotFOV + 7, ImGui::GetColorU32({ 255.0,255.0,255.0, 0.14f }), 105);
			window.DrawList->AddCircle(ImVec2(width / 2, height / 2), Settings.AimbotFOV + 8, ImGui::GetColorU32({ 255.0,255.0,255.0, 0.12f }), 105);
			window.DrawList->AddCircle(ImVec2(width / 2, height / 2), Settings.AimbotFOV + 9, ImGui::GetColorU32({ 255.0,255.0,255.0, 0.12f }), 105);
			window.DrawList->AddCircle(ImVec2(width / 2, height / 2), Settings.AimbotFOV + 10, ImGui::GetColorU32({ 255.0,255.0,255.0, 0.12f }), 105);
			window.DrawList->AddCircle(ImVec2(width / 2, height / 2), Settings.AimbotFOV + 11, ImGui::GetColorU32({ 255.0,255.0,255.0, 0.10f }), 105);
			window.DrawList->AddCircle(ImVec2(width / 2, height / 2), Settings.AimbotFOV + 12, ImGui::GetColorU32({ 255.0,255.0,255.0, 0.10f }), 105);
			window.DrawList->AddCircle(ImVec2(width / 2, height / 2), Settings.AimbotFOV + 13, ImGui::GetColorU32({ 255.0,255.0,255.0, 0.10f }), 105);
			window.DrawList->AddCircle(ImVec2(width / 2, height / 2), Settings.AimbotFOV + 14, ImGui::GetColorU32({ 255.0,255.0,255.0, 0.08f }), 105);
			window.DrawList->AddCircle(ImVec2(width / 2, height / 2), Settings.AimbotFOV + 15, ImGui::GetColorU32({ 255.0,255.0,255.0, 0.08f }), 105);
			window.DrawList->AddCircle(ImVec2(width / 2, height / 2), Settings.AimbotFOV + 16, ImGui::GetColorU32({ 255.0,255.0,255.0, 0.08f }), 105);
			window.DrawList->AddCircle(ImVec2(width / 2, height / 2), Settings.AimbotFOV + 17, ImGui::GetColorU32({ 255.0,255.0,255.0, 0.06f }), 105);
			window.DrawList->AddCircle(ImVec2(width / 2, height / 2), Settings.AimbotFOV + 18, ImGui::GetColorU32({ 255.0,255.0,255.0, 0.06f }), 105);
			window.DrawList->AddCircle(ImVec2(width / 2, height / 2), Settings.AimbotFOV + 19, ImGui::GetColorU32({ 255.0,255.0,255.0, 0.06f }), 105);
			window.DrawList->AddCircle(ImVec2(width / 2, height / 2), Settings.AimbotFOV + 20, ImGui::GetColorU32({ 255.0,255.0,255.0, 0.04f }), 105);
			window.DrawList->AddCircle(ImVec2(width / 2, height / 2), Settings.AimbotFOV + 21, ImGui::GetColorU32({ 255.0,255.0,255.0, 0.04f }), 105);
			window.DrawList->AddCircle(ImVec2(width / 2, height / 2), Settings.AimbotFOV + 22, ImGui::GetColorU32({ 255.0,255.0,255.0, 0.04f }), 105);
			window.DrawList->AddCircle(ImVec2(width / 2, height / 2), Settings.AimbotFOV + 23, ImGui::GetColorU32({ 255.0,255.0,255.0, 0.02f }), 105);
			window.DrawList->AddCircle(ImVec2(width / 2, height / 2), Settings.AimbotFOV + 24, ImGui::GetColorU32({ 255.0,255.0,255.0, 0.02f }), 105);
			window.DrawList->AddCircle(ImVec2(width / 2, height / 2), Settings.AimbotFOV + 25, ImGui::GetColorU32({ 255.0,255.0,255.0, 0.02f }), 105);
			window.DrawList->AddCircle(ImVec2(width / 2, height / 2), Settings.AimbotFOV + 26, ImGui::GetColorU32({ 255.0,255.0,255.0, 0.01f }), 105);
			window.DrawList->AddCircle(ImVec2(width / 2, height / 2), Settings.AimbotFOV + 27, ImGui::GetColorU32({ 255.0,255.0,255.0, 0.01f }), 105);
			window.DrawList->AddCircle(ImVec2(width / 2, height / 2), Settings.AimbotFOV + 28, ImGui::GetColorU32({ 255.0,255.0,255.0, 0.005f }), 105);
			window.DrawList->AddCircle(ImVec2(width / 2, height / 2), Settings.AimbotFOV, +29, ImGui::GetColorU32({ 255.0,255.0,255.0, 0.005f }), 105); {
			}
		}

		/*printf("\nLocalPlayerController : %p.", Core::LocalPlayerController);
		printf("\nSetControlRotation : %p.", Offsets::Engine::Controller::SetControlRotation);
		printf("\n Target Pawn : %p.", Core::TargetPawn);
		printf("\nClosest Pawn %p.", closestPawn);
		auto currentRotation = Util::GetViewInfo().Rotation;
		printf("\nCurrent Rotation : %p.", currentRotation);
		printf("\nClosest Pawn : %p.", closestPawn); */
		//AddMarker(window, width / 2, height / 2, 120 , pawn ,"not made by impur", ImGui::GetColorU32({ 0.75f, 0.75f, 0.75f, 1.0f }))l
		success = TRUE;
	} while (FALSE);

	if (!success) {
		Core::LocalPlayerController = Core::LocalPlayerPawn = Core::TargetPawn = nullptr;
	}
	EndScene(window);
	//// i had a sleep here :(
	return PresentOriginal(swapChain, syncInterval, flags);
}

__declspec(dllexport) HRESULT ResizeHook(IDXGISwapChain* swapChain, UINT bufferCount, UINT width, UINT height, DXGI_FORMAT newFormat, UINT swapChainFlags) {
	ImGui_ImplDX11_Shutdown();
	renderTargetView->Release();
	immediateContext->Release();
	device->Release();
	device = nullptr;

	return ResizeOriginal(swapChain, bufferCount, width, height, newFormat, swapChainFlags);
}

bool Render::Initialize() {
	IDXGISwapChain* swapChain = nullptr;
	ID3D11Device* device = nullptr;
	ID3D11DeviceContext* context = nullptr;
	auto                 featureLevel = D3D_FEATURE_LEVEL_11_0;

	DXGI_SWAP_CHAIN_DESC sd = { 0 };
	sd.BufferCount = 1;
	sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
	sd.OutputWindow = FindWindow((L"UnrealWindow"), (L"Fortnite  "));
	sd.SampleDesc.Count = 1;
	sd.Windowed = TRUE;

	if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, 0, 0, &featureLevel, 1, D3D11_SDK_VERSION, &sd, &swapChain, &device, nullptr, &context))) {
		MessageBox(0, L"Failed to create D3D11 device and swap chain", L"Failure", MB_ICONERROR);
		return FALSE;
	}

	auto table = *reinterpret_cast<PVOID**>(swapChain);
	auto present = table[8];
	auto resize = table[13];

	context->Release();
	device->Release();
	swapChain->Release();

	MH_CreateHook(present, PresentHook, reinterpret_cast<PVOID*>(&PresentOriginal));
	MH_EnableHook(present);

	MH_CreateHook(resize, ResizeHook, reinterpret_cast<PVOID*>(&ResizeOriginal));
	MH_EnableHook(resize);

	return TRUE;
}