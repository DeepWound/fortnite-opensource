#pragma once
#include "imgui/imgui.h"
#include <d3d11.h>
namespace Global
{
	bool dx_init = false;
	ImVec4 RGB;
	int my_image_width = 0, my_image_height = 0, my_bg_width = 0, my_bg_height = 0;
	ID3D11ShaderResourceView* my_texture = NULL;
	ID3D11ShaderResourceView* bg_texture = NULL;
}