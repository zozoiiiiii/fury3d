#ifndef _FURY_SCENE_H_
#define _FURY_SCENE_H_

#include <string>
#include <vector>

#include "Fury/Entity.h"

namespace fury
{
	class EntityManager;

	class SceneNode;

	class SceneManager;

	class Material; 

	class Mesh;

	class FURY_API Scene : public Entity 
	{
	public:

		typedef std::shared_ptr<Scene> Ptr;

		static Ptr Active;

		static std::string Path(const std::string &path);

		static std::shared_ptr<EntityManager> Manager();

		static Ptr Create(const std::string &name, const std::string &workingDir, const std::shared_ptr<SceneManager> &sceneManager = nullptr);

	protected:

		std::shared_ptr<SceneNode> m_RootNode;

		std::shared_ptr<SceneManager> m_SceneManager;

		std::shared_ptr<EntityManager> m_EntityManager;

		std::string m_WorkingDir;

	public:

		Scene(const std::string &name, const std::string &workingDir, const std::shared_ptr<SceneManager> &sceneManager = nullptr);

		~Scene();
		
		void Clear();

		virtual bool Load(const void* wrapper, bool object = true) override;

		virtual void Save(void* wrapper, bool object = true) override;

		std::shared_ptr<SceneNode> GetRootNode() const;

		std::shared_ptr<SceneManager> GetSceneManager() const;

		std::shared_ptr<EntityManager> GetEntityManager() const;

		// extra resources prepends this to filepath when loading.
		std::string GetWorkingDir() const;

		void SetWorkingDir(const std::string &path);
		
	};
}

#endif // _FURY_SCENE_H_