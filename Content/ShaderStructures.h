#pragma once

namespace Cemu_UWP_Host
{
	// Buffer de constantes usado para enviar matrizes MVP para o sombreador de vértice.
	struct ModelViewProjectionConstantBuffer
	{
		DirectX::XMFLOAT4X4 model;
		DirectX::XMFLOAT4X4 view;
		DirectX::XMFLOAT4X4 projection;
	};

	// Usado para enviar dados por vértice para o sombreador de vértice.
	struct VertexPositionColor
	{
		DirectX::XMFLOAT3 pos;
		DirectX::XMFLOAT3 color;
	};
}