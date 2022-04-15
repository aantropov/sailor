#pragma once
#include <cassert>
#include <memory>
#include <functional>
#include <concepts>
#include <type_traits>
#include <iterator>
#include <algorithm>
#include "Core/Defines.h"
#include "Math/Math.h"
#include "Memory/Memory.h"
#include "Containers/Concepts.h"
#include "Containers/Map.h"
#include "Containers/Vector.h"
#include "RHI/DebugContext.h"

namespace Sailor
{
	template<typename TElementType, typename TAllocator = Memory::DefaultGlobalAllocator>
	class SAILOR_API TOctree final
	{
		static constexpr uint32_t NumElementsInNode = 4u;

	protected:

		struct TBounds
		{
			TBounds() {}
			TBounds(const glm::ivec3& pos, const glm::ivec3& extents) : m_position(pos), m_extents(extents) {}

			bool operator==(const TBounds& rhs) const { return m_position == rhs.m_position || m_extents == rhs.m_extents; }

			glm::ivec3 m_position{};
			glm::ivec3 m_extents{};
		};

		struct TNode
		{
			constexpr uint32_t GetIndex(int32_t x, int32_t y, int32_t z) { return (x == -1 ? 0 : 1) + (z == -1 ? 1 : 0) * 2 + (y == -1 ? 0 : 1) * 4; }

			__forceinline bool IsLeaf() const { return m_internal[0] == nullptr; }
			__forceinline bool Contains(const glm::ivec3& pos, const glm::ivec3& extents) const
			{
				const int32_t halfSize = m_size / 2;
				return m_center.x - halfSize < pos.x - extents.x &&
					m_center.y - halfSize < pos.y - extents.y &&
					m_center.z - halfSize < pos.z - extents.z &&
					m_center.x + halfSize > pos.x + extents.x &&
					m_center.y + halfSize > pos.y + extents.y &&
					m_center.z + halfSize > pos.z + extents.z;
			}

			__forceinline bool Overlaps(const glm::ivec3& pos, const glm::ivec3& extents) const
			{
				const int32_t halfSize = m_size / 2;
				return
					(m_center.x - halfSize < pos.x + extents.x) && (m_center.x + halfSize > pos.x - extents.x) &&
					(m_center.y - halfSize < pos.y + extents.y) && (m_center.y + halfSize > pos.y - extents.y) &&
					(m_center.z - halfSize < pos.z + extents.z) && (m_center.z + halfSize > pos.z - extents.z);
			}

			__forceinline bool CanCollapse() const
			{
				if (IsLeaf())
				{
					return false;
				}

				for (uint32_t i = 0; i < 8; i++)
				{
					if (!m_internal[i]->IsLeaf() || m_internal[i]->m_elements.Num() > 0)
					{
						return false;
					}
				}

				return true;
			}

			__forceinline void Insert(const TElementType& element, const glm::ivec3& pos, const glm::ivec3& extents)
			{
				m_elements[element] = TBounds(pos, extents);
			}

			__forceinline bool Remove(const TElementType& element)
			{
				return m_elements.Remove(element);
			}

			uint32_t m_size = 0;
			glm::ivec3 m_center{};
			TNode* m_internal[8]{};

			TMap<TElementType, TBounds> m_elements{};
		};

	public:

		// Constructors & Destructor
		TOctree(glm::ivec3 center = glm::ivec3(0, 0, 0), uint32_t size = 16536u, uint32_t minSize = 4) : m_map(size)
		{
			m_minSize = minSize;
			m_root = Memory::New<TNode>(m_allocator);
			m_root->m_size = size;
			m_root->m_center = center;
		}

		virtual ~TOctree()
		{
			Clear();

			Memory::Delete<TNode>(m_allocator, m_root);
			m_root = nullptr;
		}

		TOctree(const TOctree& octree) {}
		TOctree& operator= (const TOctree& octree) {}

		TOctree(TOctree&& octree) { Swap(std::move(*this), octree); }
		TOctree& operator= (TOctree&& octree)
		{
			Swap(std::move(*this), octree);
			return this;
		}

		static void Swap(TOctree&& lhs, TOctree&& rhs)
		{
			std::swap(rhs->m_root, lhs->m_root);
			std::swap(rhs->m_allocator, lhs->m_allocator);
			std::swap(rhs->m_num, lhs->m_num);
			std::swap(rhs->m_minSize, lhs->m_minSize);
			std::swap(rhs->m_map, lhs->m_map);
		}

		void Clear()
		{
			if (m_root)
			{
				Clear_Internal(m_root);
			}
			m_map.Clear();
		}

		bool Contains(const TElementType& element) const { return m_root && m_map.ContainsKey(element); }
		size_t Num() const { return m_num; }
		size_t NumNodes() const { return m_numNodes; }

		//TVector<TElementType, TAllocator> GetElements(glm::ivec3 extents) const { return TVector(); }

		bool Insert(const glm::ivec3& pos, const glm::ivec3& extents, const TElementType& element)
		{
			if (Insert_Internal(m_root, pos, extents, element))
			{
				m_num++;
				return true;
			}
			return false;
		}

		bool Remove(const TElementType& element)
		{
			TNode** node;
			if (m_map.Find(element, node))
			{
				if ((*node)->Remove(element))
				{
					Resolve_Internal(*node);
					m_map.Remove(element);

					return true;
				}
			}
			return false;
		}

		__forceinline void Resolve()
		{
			Resolve_Internal(m_root);
		}

		void DrawOctree(RHI::DebugContext& context, float duration = 0.0f) const
		{
			DrawOctree_Internal(m_root, context, duration);
		}

	protected:

		void DrawOctree_Internal(const TNode* node, RHI::DebugContext& context, float duration = 0.0f) const
		{
			if (!node->IsLeaf())
			{
				for (uint32_t i = 0; i < 8; i++)
				{
					DrawOctree_Internal(node->m_internal[i], context, duration);
				}
			}

			if (node->IsLeaf() && node->m_elements.Num() == 0)
			{
				return;
			}

			Math::AABB aabb;
			aabb.m_min = glm::vec3(node->m_center) - (float)node->m_size * glm::vec3(0.5f, 0.5f, 0.5f);
			aabb.m_max = glm::vec3(node->m_center) + (float)node->m_size * glm::vec3(0.5f, 0.5f, 0.5f);

			context.DrawAABB(aabb, glm::vec4(0.2f, 1.0f, 0.2f, 1.0f), duration);

			for (auto& el : node->m_elements)
			{
				Math::AABB aabb;
				aabb.m_min = glm::vec3(el.m_second.m_position - el.m_second.m_extents);
				aabb.m_max = glm::vec3(el.m_second.m_position + el.m_second.m_extents);

				const auto color = glm::vec4((0x4 & reinterpret_cast<size_t>(node)) / 16.0f,
					(0x4 & reinterpret_cast<size_t>(node) >> 4) / 16.0f,
					(0x4 & reinterpret_cast<size_t>(node) >> 8) / 16.0f,
					(0x4 & reinterpret_cast<size_t>(node) >> 12) / 16.0f);

				context.DrawAABB(aabb, color, duration);
			}
		}

		void Resolve_Internal(TNode* node)
		{
			if (node->m_elements.Num())
			{
				return;
			}

			for (uint32_t i = 0; i < 8; i++)
			{
				if (!node->IsLeaf())
				{
					Resolve_Internal(node->m_internal[i]);
				}
			}

			if (node->CanCollapse())
			{
				Collapse(node);
			}
		}

		void Clear_Internal(TNode* node)
		{
			if (node->IsLeaf())
			{
				return;
			}

			for (uint32_t i = 0; i < 8; i++)
			{
				Clear_Internal(node->m_internal[i]);
			}

			Collapse(node);
		}

		__forceinline void Subdivide(TNode* node)
		{
			assert(node->IsLeaf());

			// Bottom   Top    
			// |0|1|    |4|5|
			// |2|3|    |6|7|
			const glm::ivec3 offset[] = { glm::ivec3(1, -1, -1), glm::ivec3(1, -1, 1), glm::ivec3(-1, -1, -1), glm::ivec3(-1, -1, 1),
										  glm::ivec3(1, 1, -1), glm::ivec3(1, 1, 1), glm::ivec3(-1, 1, -1), glm::ivec3(-1, 1, 1) };

			const int32_t quarterSize = node->m_size / 4;

			for (uint32_t i = 0; i < 8; i++)
			{
				node->m_internal[i] = Memory::New<TNode>(m_allocator);
				node->m_internal[i]->m_size = quarterSize * 2;
				node->m_internal[i]->m_center = offset[i] * quarterSize + node->m_center;
			}

			m_numNodes += 8;
		}

		__forceinline void Collapse(TNode* node)
		{
			for (uint32_t i = 0; i < 8; i++)
			{
				Memory::Delete<TNode>(m_allocator, node->m_internal[i]);
				node->m_internal[i] = nullptr;
			}

			m_numNodes -= 8;
		}

		// Not used
		bool Remove_Internal(TNode* node, const glm::ivec3& pos, const glm::ivec3& extents, const TElementType& element)
		{
			const bool bIsLeaf = node->IsLeaf();
			const bool bOverlaps = node->Overlaps(pos, extents);

			if (!bOverlaps)
			{
				return false;
			}

			const size_t index = node->m_elements.FindIf([&](const auto& lhs) { return lhs.m_element == element; });
			if (index != -1)
			{
				node->m_elements.RemoveAtSwap(index);
				Resolve_Internal(node);
				return true;
			}

			if (!bIsLeaf)
			{
				for (uint32_t i = 0; i < 8; i++)
				{
					if (Remove_Internal(node->m_internal[i], pos, extents, element))
					{
						return true;
					}
				}
			}
			return false;
		}

		bool Insert_Internal(TNode* node, const glm::ivec3& pos, const glm::ivec3& extents, const TElementType& element)
		{
			const bool bIsLeaf = node->IsLeaf();
			const bool bContains = node->Contains(pos, extents);

			if (!bContains)
			{
				return false;
			}

			if (bIsLeaf)
			{
				if (node->m_elements.Num() == NumElementsInNode && node->m_size > m_minSize)
				{
					node->Insert(element, pos, extents);

					auto elements = std::move(node->m_elements);
					node->m_elements.Clear();

					Subdivide(node);

					for (auto& el : elements)
					{
						assert(Insert_Internal(node, el.m_second.m_position, el.m_second.m_extents, el.m_first));
					}

					return true;
				}

				node->Insert(element, pos, extents);
				m_map[element] = node;

				return true;
			}
			else
			{
				for (uint32_t i = 0; i < 8; i++)
				{
					if (Insert_Internal(node->m_internal[i], pos, extents, element))
					{
						return true;
					}
				}

				node->Insert(element, pos, extents);
				m_map[element] = node;
			}

			return true;
		}

		TNode* m_root{};
		size_t m_num = 0u;
		uint32_t m_minSize = 1;
		size_t m_numNodes = 1u;
		TAllocator m_allocator{};
		TMap<TElementType, TNode*> m_map{};
	};

	SAILOR_API void RunOctreeBenchmark();
}