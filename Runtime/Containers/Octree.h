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
#include "Containers/Vector.h"

namespace Sailor
{
	template<typename TElementType, typename TAllocator = Memory::DefaultGlobalAllocator>
	class SAILOR_API TOctree final
	{
		static constexpr uint32_t NumElementsInNode = 12u;

	protected:

		struct TNodeElement
		{
			TNodeElement(TElementType&& data, const glm::ivec3& pos, const glm::ivec3& extents) : m_element(std::move(data)), m_extents(extents), m_position(pos) {}
			TNodeElement(const TElementType& data, const glm::ivec3& pos, const glm::ivec3& extents) : m_element(data), m_extents(extents), m_position(pos) {}

			TElementType m_element{};
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

			TNode* m_internal[8]{};
			uint32_t m_size = 0;
			glm::ivec3 m_center{};
			//TVector<TNodeElement, TInlineAllocator<NumElementsInNode * sizeof(TNodeElement*), TAllocator>> m_elements{};
			TVector<TNodeElement, TAllocator> m_elements{};
		};

	public:

		// Constructors & Destructor
		TOctree(glm::ivec3 center, uint32_t size)
		{
			m_root = m_allocator.New<TNode>();

			m_root->m_size = size;
			m_root->m_center = center;
		}

		virtual ~TOctree() { Clear(); }

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
		}

		void Clear() {}

		size_t GetNum() const { return m_num; }

		//TVector<TElementType, TAllocator> GetElements(glm::ivec3 extents) const { return TVector(); }

		void Insert(const glm::ivec3& pos, const glm::ivec3& extents, const TElementType& element)
		{
			Insert_Internal(m_root, pos, extents, element);
		}

		bool Remove(const glm::ivec3& pos, const glm::ivec3& extents, const TElementType& element)
		{

			return true;
		}

	protected:

		void Subdivide(TNode* node)
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
				node->m_internal[i] = static_cast<TNode*>(m_allocator.Allocate(sizeof(TNode)));
				node->m_internal[i]->m_size = quarterSize * 2;
				node->m_internal[i]->m_center = offset[i] * quarterSize + node->m_center;
			}
		}

		bool Remove_Internal(TNode* node, const glm::ivec3& pos, const glm::ivec3& extents, const TElementType& element)
		{
			const bool bIsLeaf = node->m_elements.Num();
			const bool bOverlap = node->Contains(pos, extents);

			if (!bOverlap)
			{
				return false;
			}

			//if(node->m_elements.RemoveSwap(node))
		}

		bool Insert_Internal(TNode* node, const glm::ivec3& pos, const glm::ivec3& extents, const TElementType& element)
		{
			const bool bIsLeaf = node->m_elements.Num();
			const bool bOverlap = node->Contains(pos, extents);

			if (!bOverlap)
			{
				return false;
			}

			if (bIsLeaf)
			{
				if (node->m_elements.Num() < NumElementsInNode)
				{
					node->m_elements.Add(TNodeElement(element, pos, extents));
					return true;
				}
				else if (node->m_elements.Num() == NumElementsInNode)
				{
					auto elements = std::move(node->m_elements);
					node->m_elements.Clear();

					Subdivide(node);

					for (auto& el : elements)
					{
						assert(Insert_Internal(node, el.m_position, el.m_extents, el.m_element));
					}
				}

				return Insert_Internal(node, pos, extents, element);
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
				node->m_elements.Add(TNodeElement(element, pos, extents));
			}

			return true;
		}

		TNode* m_root{};
		size_t m_num = 0u;
		TAllocator m_allocator{};
	};

	SAILOR_API void RunOctreeBenchmark();
}