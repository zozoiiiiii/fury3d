#include <array>
#include <cmath>
#include <unordered_map>

#include "Camera.h"
#include "Log.h"
#include "EnumUtil.h"
#include "EntityUtil.h"
#include "Frustum.h"
#include "GLLoader.h"
#include "Light.h"
#include "MathUtil.h"
#include "Material.h"
#include "Mesh.h"
#include "MeshRender.h"
#include "MeshUtil.h"
#include "Pass.h"
#include "PrelightPipeline.h"
#include "RenderQuery.h"
#include "RenderUtil.h"
#include "SceneManager.h"
#include "SceneNode.h"
#include "Shader.h"
#include "SphereBounds.h"
#include "Texture.h"

namespace fury
{
	PrelightPipeline::Ptr PrelightPipeline::Create(const std::string &name)
	{
		return std::make_shared<PrelightPipeline>(name);
	}

	PrelightPipeline::PrelightPipeline(const std::string &name)
		: Pipeline(name)
	{
		m_TypeIndex = typeid(PrelightPipeline);
	}

	void PrelightPipeline::Execute(const std::shared_ptr<SceneManager> &sceneManager)
	{
		// pre
		m_CurrentCamera = nullptr;
		m_CurrentShader = nullptr;
		SortPassByIndex();

		// find visible nodes, 1 cam 1 query
		std::unordered_map<std::string, RenderQuery::Ptr> queries;

		for (auto pair : m_PassMap)
		{
			auto pass = pair.second;
			auto camNode = pass->GetCameraNode();

			if (camNode == nullptr)
			{
				FURYW << "Camera for pass " + pass->GetName() + " not found!";
				continue;
			}

			auto it = queries.find(camNode->GetName());
			if (it != queries.end())
				continue;

			RenderQuery::Ptr query = RenderQuery::Create();

			sceneManager->GetRenderQuery(camNode->GetComponent<Camera>()->GetFrustum(), query);
			query->Sort(camNode->GetWorldPosition());

			queries.emplace(camNode->GetName(), query);
		}

		// draw passes

		for (unsigned int i = 0; i < m_SortedPasses.size(); i++)
		{
			auto passName = m_SortedPasses[i];
			auto pass = m_PassMap[passName];

			auto drawMode = pass->GetDrawMode();

			m_CurrentCamera = pass->GetCameraNode();
			m_CurrentShader = pass->GetFirstShader();

			if (m_CurrentCamera == nullptr)
				continue;

			auto query = queries[m_CurrentCamera->GetName()];

			if (drawMode == DrawMode::OPAQUE)
			{
				pass->Bind();
				for (const auto &unit : query->opaqueUnits)
					DrawUnit(pass, unit);
				pass->UnBind();
			}
			else if (drawMode == DrawMode::TRANSPARENT)
			{
				pass->Bind();
				for (const auto &unit : query->transparentUnits)
					DrawUnit(pass, unit);
				pass->UnBind();
			}
			else if (drawMode == DrawMode::QUAD)
			{
				pass->Bind();
				DrawQuad(pass);
				pass->UnBind();
			}
			else if (drawMode == DrawMode::LIGHT)
			{
				pass->Bind(true);
				//glDepthMask(GL_FALSE);

				for (const auto &node : query->lightNodes)
				{
					if (auto ptr = node->GetComponent<Light>())
					{
						if (ptr->GetType() == LightType::DIRECTIONAL)
							DrawDirLight(sceneManager, pass, node);
						else if (ptr->GetType() == LightType::POINT)
							DrawPointLight(sceneManager, pass, node);
						else
							DrawSpotLight(sceneManager, pass, node);
					}
				}
				//DrawLight(sceneManager, pass, node);

				//glDepthMask(GL_TRUE);
			}
		}

		// draw debug
		if (m_DebugFlags != 0)
			DrawDebug(queries);

		// post
		m_CurrentCamera = nullptr;
		m_CurrentShader = nullptr;
	}

	void PrelightPipeline::DrawUnit(const std::shared_ptr<Pass> &pass, const RenderUnit &unit)
	{
		auto node = unit.node;
		auto mesh = unit.mesh;
		auto material = unit.material;

		auto shader = material->GetShaderForPass(pass->GetRenderIndex());

		if (shader == nullptr)
			shader = pass->GetShader(mesh->IsSkinnedMesh() ? ShaderType::SKINNED_MESH : ShaderType::STATIC_MESH,
			material->GetTextureFlags());

		if (shader == nullptr)
		{
			FURYW << "Failed to draw " << node->GetName() << ", shader not found!";
			return;
		}

		shader->Bind();
		shader->BindCamera(m_CurrentCamera);
		shader->BindMatrix(Matrix4::WORLD_MATRIX, node->GetWorldMatrix());

		shader->BindMaterial(material);

		for (unsigned int i = 0; i < pass->GetTextureCount(true); i++)
		{
			auto ptr = pass->GetTextureAt(i, true);
			shader->BindTexture(ptr->GetName(), ptr);
		}

		if (mesh->GetSubMeshCount() > 0)
		{
			auto subMesh = mesh->GetSubMeshAt(unit.subMesh);
			shader->BindSubMesh(mesh, unit.subMesh);
			glDrawElements(GL_TRIANGLES, subMesh->Indices.Data.size(), GL_UNSIGNED_INT, 0);

			RenderUtil::Instance()->IncreaseTriangleCount(subMesh->Indices.Data.size());
		}
		else
		{
			shader->BindMesh(mesh);
			glDrawElements(GL_TRIANGLES, mesh->Indices.Data.size(), GL_UNSIGNED_INT, 0);

			RenderUtil::Instance()->IncreaseTriangleCount(mesh->Indices.Data.size());
		}

		shader->UnBind();

		// TODO: Maybe subMeshCount ? 
		if (mesh->IsSkinnedMesh())
			RenderUtil::Instance()->IncreaseSkinnedMeshCount();
		else
			RenderUtil::Instance()->IncreaseMeshCount();

		RenderUtil::Instance()->IncreaseDrawCall();
	}

	void PrelightPipeline::DrawPointLight(const std::shared_ptr<SceneManager> &sceneManager, const std::shared_ptr<Pass> &pass, const std::shared_ptr<SceneNode> &node)
	{
		auto light = node->GetComponent<Light>();
		auto camPtr = m_CurrentCamera->GetComponent<Camera>();
		auto camPos = m_CurrentCamera->GetWorldPosition();
		auto mesh = light->GetMesh();
		auto worldMatrix = node->GetWorldMatrix();

		Shader::Ptr shader = nullptr;
		bool castShadows = light->GetCastShadows();

		// find correct shader.
		shader = GetShaderByName(castShadows ? "pointlight_shadow_shader" : "pointlight_shader");
		if (shader == nullptr)
		{
			FURYW << "Shader for light " << node->GetName() << " not found!";
			return;
		}

		// draw shadowMap if we castShadows.
		std::pair<Texture::Ptr, Matrix4> shadowData;
		if (castShadows)
			shadowData = DrawPointLightShadowMap(sceneManager, pass, node);

		// ready to draw light volumn
		pass->Bind(false);

		// change depthTest && face culling state.
		{
			float camNear = (camPtr->GetFrustum().GetCurrentCorners()[0] - camPos).Length();
			if (SphereBounds(node->GetWorldPosition(), light->GetRadius() + camNear).IsInsideFast(camPos))
			{
				glDisable(GL_DEPTH_TEST);
				glCullFace(GL_FRONT);
			}
			else
			{
				glEnable(GL_DEPTH_TEST);
				glCullFace(GL_BACK);
			}

			worldMatrix.AppendScale(Vector4(light->GetRadius(), 0.0f));
		}
		
		shader->Bind();

		shader->BindCamera(m_CurrentCamera);
		shader->BindMatrix(Matrix4::WORLD_MATRIX, worldMatrix);

		if (castShadows && shadowData.first != nullptr)
		{
			shader->BindTexture("shadow_buffer", shadowData.first);
			shader->BindMatrix("shadow_matrix", &shadowData.second.Raw[0]);
		}

		shader->BindLight(node);
		shader->BindMesh(mesh);

		for (unsigned int i = 0; i < pass->GetTextureCount(true); i++)
		{
			auto ptr = pass->GetTextureAt(i, true);
			shader->BindTexture(ptr->GetName(), ptr);
		}

		glDrawElements(GL_TRIANGLES, mesh->Indices.Data.size(), GL_UNSIGNED_INT, 0);

		shader->UnBind();

		RenderUtil::Instance()->IncreaseDrawCall();
		RenderUtil::Instance()->IncreaseLightCount();

		pass->UnBind();

		// collect used shadow buffer
		if (castShadows)
			Texture::Pool.Collect(shadowData.first);
	}

	void PrelightPipeline::DrawDirLight(const std::shared_ptr<SceneManager> &sceneManager, const std::shared_ptr<Pass> &pass, const std::shared_ptr<SceneNode> &node)
	{
		auto light = node->GetComponent<Light>();
		auto camPtr = m_CurrentCamera->GetComponent<Camera>();
		auto camPos = m_CurrentCamera->GetWorldPosition();
		auto mesh = light->GetMesh();
		auto worldMatrix = node->GetWorldMatrix();

		Shader::Ptr shader = nullptr;
		bool castShadows = light->GetCastShadows();

		// find correct shader.
		shader = GetShaderByName(castShadows ? "dirlight_csm_shader" : "dirlight_shader");
		if (shader == nullptr)
		{
			FURYW << "Shader for light " << node->GetName() << " not found!";
			return;
		}

		// draw shadowMap if we castShadows.
		std::pair<Texture::Ptr, std::vector<Matrix4>> shadowData;
		if (castShadows)
			shadowData = DrawCascadedShadowMap(sceneManager, pass, node);

		// ready to draw light volumn
		pass->Bind(false);

		// change depthTest && face culling state.
		glEnable(GL_DEPTH_TEST);
		glCullFace(GL_BACK);

		shader->Bind();

		shader->BindCamera(m_CurrentCamera);
		shader->BindMatrix(Matrix4::WORLD_MATRIX, worldMatrix);

		if (castShadows && shadowData.first != nullptr)
		{
			shader->BindTexture("shadow_buffer", shadowData.first);

			// for cacasded shadow maps
			shader->BindMatrices("shadow_matrix", shadowData.second.size(), &shadowData.second[0]);
			float base = camPtr->GetFar() - camPtr->GetNear();
			float average = base / 4.0f;
			shader->BindFloat("shadow_far", average, average * 2, average * 3, average * 4);
		}

		shader->BindLight(node);
		shader->BindMesh(mesh);

		for (unsigned int i = 0; i < pass->GetTextureCount(true); i++)
		{
			auto ptr = pass->GetTextureAt(i, true);
			shader->BindTexture(ptr->GetName(), ptr);
		}

		glDrawElements(GL_TRIANGLES, mesh->Indices.Data.size(), GL_UNSIGNED_INT, 0);

		shader->UnBind();

		RenderUtil::Instance()->IncreaseDrawCall();
		RenderUtil::Instance()->IncreaseLightCount();

		pass->UnBind();

		// collect used shadow buffer
		if (castShadows)
			Texture::Pool.Collect(shadowData.first);
	}

	void PrelightPipeline::DrawSpotLight(const std::shared_ptr<SceneManager> &sceneManager, const std::shared_ptr<Pass> &pass, const std::shared_ptr<SceneNode> &node)
	{
		auto light = node->GetComponent<Light>();
		auto camPtr = m_CurrentCamera->GetComponent<Camera>();
		auto camPos = m_CurrentCamera->GetWorldPosition();
		auto mesh = light->GetMesh();
		auto worldMatrix = node->GetWorldMatrix();

		Shader::Ptr shader = nullptr;
		bool castShadows = light->GetCastShadows();

		// find correct shader.
		shader = GetShaderByName(castShadows ? "spotlight_shadow_shader" : "spotlight_shader");
		if (shader == nullptr)
		{
			FURYW << "Shader for light " << node->GetName() << " not found!";
			return;
		}

		// draw shadowMap if we castShadows.
		std::pair<Texture::Ptr, Matrix4> shadowData;
		if (castShadows)
			shadowData = DrawSpotLightShadowMap(sceneManager, pass, node);

		// ready to draw light volumn
		pass->Bind(false);

		// change depthTest && face culling state.
		{
			auto coneCenter = node->GetWorldPosition();
			auto coneDir = worldMatrix.Multiply(Vector4(0, -1, 0, 0)).Normalized();

			float camNear = (camPtr->GetFrustum().GetCurrentCorners()[0] - camPos).Length();
			float theta = light->GetOutterAngle() * 0.5f;
			float height = light->GetRadius();
			float extra = camNear / std::sin(theta);

			coneCenter = coneCenter - coneDir * extra;
			height += camNear + extra;

			if (MathUtil::PointInCone(coneCenter, coneDir, height, theta, camPos))
			{
				glDisable(GL_DEPTH_TEST);
				glCullFace(GL_FRONT);
			}
			else
			{
				glEnable(GL_DEPTH_TEST);
				glCullFace(GL_BACK);
			}
		}

		shader->Bind();

		shader->BindCamera(m_CurrentCamera);
		shader->BindMatrix(Matrix4::WORLD_MATRIX, worldMatrix);

		if (castShadows && shadowData.first != nullptr)
		{
			shader->BindTexture("shadow_buffer", shadowData.first);
			shader->BindMatrix("shadow_matrix", &shadowData.second.Raw[0]);
		}

		shader->BindLight(node);
		shader->BindMesh(mesh);

		for (unsigned int i = 0; i < pass->GetTextureCount(true); i++)
		{
			auto ptr = pass->GetTextureAt(i, true);
			shader->BindTexture(ptr->GetName(), ptr);
		}

		glDrawElements(GL_TRIANGLES, mesh->Indices.Data.size(), GL_UNSIGNED_INT, 0);

		shader->UnBind();

		RenderUtil::Instance()->IncreaseDrawCall();
		RenderUtil::Instance()->IncreaseLightCount();

		pass->UnBind();

		// collect used shadow buffer
		if (castShadows)
			Texture::Pool.Collect(shadowData.first);
	}

	void PrelightPipeline::DrawQuad(const std::shared_ptr<Pass> &pass)
	{
		auto shader = m_CurrentShader;
		auto mesh = MeshUtil::GetUnitQuad();

		if (shader == nullptr)
		{
			FURYW << "Failed to draw full screen quad, shader not found!";
			return;
		}

		shader->Bind();

		shader->BindMesh(mesh);
		shader->BindCamera(m_CurrentCamera);

		for (unsigned int i = 0; i < pass->GetTextureCount(true); i++)
		{
			auto ptr = pass->GetTextureAt(i, true);
			shader->BindTexture(ptr->GetName(), ptr);
		}

		glDrawElements(GL_TRIANGLES, mesh->Indices.Data.size(), GL_UNSIGNED_INT, 0);

		shader->UnBind();

		RenderUtil::Instance()->IncreaseDrawCall();
		RenderUtil::Instance()->IncreaseTriangleCount(mesh->Indices.Data.size());
	}
}