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
#include "RHI/DebugContext.h"

// Sparse octree: child nodes are allocated only for occupied octants.
namespace Sailor
{
	template<typename TElementType, typename TAllocator = Memory::DefaultGlobalAllocator>
	class TOctree2
	{
		static constexpr uint32_t NumElementsInNode = 8u;

	protected:

		struct TBounds
		{
			TBounds() = default;
			TBounds(const glm::ivec3& pos, const glm::ivec3& extents) : m_position(pos), m_extents(extents) {}

			bool operator==(const TBounds& rhs) const { return m_position == rhs.m_position && m_extents == rhs.m_extents; }

			glm::ivec3 m_position{};
			glm::ivec3 m_extents{};
		};

		struct TNode
		{
			// Bottom   Top
			// |0|1|    |4|5|
			// |2|3|    |6|7|
			__forceinline constexpr uint32_t GetIndex(int32_t x, int32_t y, int32_t z) const { return (z < 0 ? 0 : 1) + (x < 0 ? 1 : 0) * 2 + (y < 0 ? 0 : 1) * 4; }

			glm::ivec3 GetChildCenter(size_t index) const
			{
				static const glm::ivec3 offsets[] = {
					{ 1, -1, -1 }, { 1, -1, 1 }, { -1, -1, -1 }, { -1, -1, 1 },
					{ 1, 1, -1 }, { 1, 1, 1 }, { -1, 1, -1 }, { -1, 1, 1 }
				};
				return m_center + offsets[index] * static_cast<int32_t>(m_size / 4);
			}

			__forceinline bool IsLeaf() const { return m_bIsLeaf; }
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
					if (m_internal[i] && (!m_internal[i]->IsLeaf() || m_internal[i]->m_elements.Num() > 0))
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

			uint32_t m_size = 1;
			glm::ivec3 m_center{};
			TNode* m_internal[8]{};
			bool m_bIsLeaf = true;
			TMap<TElementType, TBounds> m_elements{ NumElementsInNode };
		};

	public:

		// Constructors & Destructor
		TOctree2(glm::ivec3 center = glm::ivec3(0, 0, 0), uint32_t size = 16536u, uint32_t minSize = 4,
			uint32_t initialCapacity = 64u) : m_map(initialCapacity)
		{
			m_minSize = minSize;
			m_root = Memory::New<TNode>(m_allocator);
			m_numNodes = 1u;
			m_root->m_size = size;
			m_root->m_center = center;
		}

		virtual ~TOctree2()
		{
			Clear();

			Memory::Delete<TNode>(m_allocator, m_root);
			m_root = nullptr;
		}

		TOctree2(const TOctree2& octree) = delete;
		TOctree2& operator= (const TOctree2& octree) = delete;

		TOctree2(TOctree2&& octree) noexcept { Swap(*this, octree); }
		TOctree2& operator= (TOctree2&& octree)
		{
			Swap(*this, octree);
			return *this;
		}

		static void Swap(TOctree2& lhs, TOctree2& rhs)
		{
			std::swap(rhs.m_root, lhs.m_root);
			std::swap(rhs.m_allocator, lhs.m_allocator);
			std::swap(rhs.m_num, lhs.m_num);
			std::swap(rhs.m_numNodes, lhs.m_numNodes);
			std::swap(rhs.m_minSize, lhs.m_minSize);
			TMap<TElementType, TNode*>::Swap(lhs.m_map, rhs.m_map);
		}

		void Clear()
		{
			m_num = 0;
			if (m_root)
			{
				Clear_Internal(*m_root);
			}
			m_map.Clear();
		}

		bool Contains(const TElementType& element) const { return m_root && m_map.ContainsKey(element); }
		size_t Num() const { return m_num; }
		size_t NumNodes() const { return m_numNodes; }

		//TVector<TElementType, TAllocator> GetElements(glm::ivec3 extents) const { return TVector(); }

		bool Insert(const glm::ivec3& pos, const glm::ivec3& extents, const TElementType& element)
		{
			if (m_root && !Contains(element) && Insert_Internal(*m_root, pos, extents, element))
			{
				m_num++;
				return true;
			}
			return false;
		}

		bool Update(const glm::ivec3& pos, const glm::ivec3& extents, const TElementType& element)
		{
			TNode** node;
			if (m_map.Find(element, node))
			{
				// If we're still in the octant then we just update the bounds
				if ((*node)->Contains(pos, extents))
				{
					(*node)->m_elements[element] = TBounds(pos, extents);
					return true;
				}

				Remove(element);
			}

			return Insert(pos, extents, element);
		}

		bool Remove(const TElementType& element)
		{
			TNode** node;
			if (m_map.Find(element, node))
			{
				if ((*node)->Remove(element))
				{
					Resolve_Internal(**node);
					m_map.Remove(element);
					m_num--;

					return true;
				}
			}
			return false;
		}

		__forceinline void Resolve()
		{
			if (m_root)
			{
				Resolve_Internal(*m_root);
			}
		}

		void DrawOctree(RHI::DebugContext& context, float duration = 0.0f) const
		{
			if (m_root)
			{
				DrawOctree_Internal(*m_root, context, duration);
			}
		}

	protected:

		void DrawOctree_Internal(const TNode& node, RHI::DebugContext& context, float duration = 0.0f) const
		{
			if (!node.IsLeaf())
			{
				for (uint32_t i = 0; i < 8; i++)
				{
					if (node.m_internal[i])
					{
						DrawOctree_Internal(*node.m_internal[i], context, duration);
					}
				}
			}

			glm::vec4 color = glm::vec4(0.2f, 1.0f, 0.2f, 1.0f);

			if (node.IsLeaf() && node.m_elements.Num() == 0)
			{
				color = glm::vec4(1.0f, 0.2f, 0.2f, 1.0f);
				//return;
			}

			Math::AABB aabb;
			aabb.m_min = glm::vec3(node.m_center) - (float)node.m_size * glm::vec3(0.5f, 0.5f, 0.5f);
			aabb.m_max = glm::vec3(node.m_center) + (float)node.m_size * glm::vec3(0.5f, 0.5f, 0.5f);

			context.DrawAABB(aabb, color, duration);

			for (const auto& el : node.m_elements)
			{
				Math::AABB aabb;
				aabb.m_min = glm::vec3(el.m_second->m_position - el.m_second->m_extents);
				aabb.m_max = glm::vec3(el.m_second->m_position + el.m_second->m_extents);

				const auto color = glm::vec4((0x4 & reinterpret_cast<size_t>(&node)) / 16.0f,
					(0x4 & reinterpret_cast<size_t>(&node) >> 4) / 16.0f,
					(0x4 & reinterpret_cast<size_t>(&node) >> 8) / 16.0f,
					(0x4 & reinterpret_cast<size_t>(&node) >> 12) / 16.0f);

				context.DrawAABB(aabb, color, duration);
			}
		}

		void Resolve_Internal(TNode& node)
		{
			if (!node.IsLeaf())
			{
				for (uint32_t i = 0; i < 8; i++)
				{
					if (node.m_internal[i])
					{
						Resolve_Internal(*node.m_internal[i]);
					}
				}
			}

			if (node.CanCollapse())
			{
				Collapse(node);
			}
		}

		void Clear_Internal(TNode& node)
		{
			node.m_elements.Clear();

			if (node.IsLeaf())
			{
				return;
			}

			for (uint32_t i = 0; i < 8; i++)
			{
				if (node.m_internal[i] != nullptr)
				{
					Clear_Internal(*node.m_internal[i]);
				}
			}

			Collapse(node);
		}

		__forceinline TNode& GetOrAddChildNode(TNode& node, size_t i)
		{
			const int32_t quarterSize = node.m_size / 4;

			if (!node.m_internal[i])
			{
				node.m_internal[i] = Memory::New<TNode>(m_allocator);
				node.m_internal[i]->m_size = quarterSize * 2;
				node.m_internal[i]->m_center = node.GetChildCenter(i);
				node.m_bIsLeaf = false;
				m_numNodes += 1;
			}

			return *node.m_internal[i];
		}

		__forceinline void Collapse(TNode& node)
		{
			for (uint32_t i = 0; i < 8; i++)
			{
				if (node.m_internal[i] == nullptr)
				{
					continue;
				}

				m_numNodes -= 1;
				Memory::Delete<TNode>(m_allocator, node.m_internal[i]);
				node.m_internal[i] = nullptr;
			}

			node.m_bIsLeaf = true;
		}

		bool Insert_Internal(TNode& node, const glm::ivec3& pos, const glm::ivec3& extents, const TElementType& element)
		{
			if (!node.Contains(pos, extents))
			{
				return false;
			}

			if (node.IsLeaf())
			{
				node.Insert(element, pos, extents);
				m_map[element] = &node;
				if (node.m_elements.Num() >= NumElementsInNode && node.m_size > m_minSize && node.m_size >= 4)
				{
					auto elements = std::move(node.m_elements);
					node.m_bIsLeaf = false;
					for (const auto& el : elements)
					{
						Insert_Internal(node, el.m_second->m_position, el.m_second->m_extents, el.m_first);
					}
				}
				return true;
			}

			const glm::ivec3 delta = pos - node.m_center;
			const size_t childIndex = node.GetIndex(delta.x, delta.y, delta.z);
			const glm::ivec3 childDelta = pos - node.GetChildCenter(childIndex);
			const glm::ivec3 childExtents(node.m_size / 4);
			if (glm::all(glm::lessThan(glm::abs(childDelta) + extents, childExtents)))
			{
				return Insert_Internal(GetOrAddChildNode(node, childIndex), pos, extents, element);
			}

			// Bounds spanning octants stay in their parent.
			node.Insert(element, pos, extents);
			m_map[element] = &node;
			return true;
		}

		TNode* m_root{};
		size_t m_num = 0u;
		uint32_t m_minSize = 1;
		size_t m_numNodes = 0u;
		TAllocator m_allocator{};
		TMap<TElementType, TNode*> m_map{};
	};

}
