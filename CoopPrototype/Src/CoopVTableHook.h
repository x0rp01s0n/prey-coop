#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <utility>

//! Per-object vtable hook with explicit ownership and restoration.
//!
//! The clone geometry intentionally matches Chairloader's existing VTableHook
//! implementation so replacing that helper does not change the live hook's
//! memory layout. Only methods present in the copied range may be replaced.
class CoopVTableHook final
{
public:
	CoopVTableHook() = default;
	~CoopVTableHook() noexcept
	{
		if (!UnhookObject() && m_memory)
		{
			// A later hook owns the object's current vtable. Do not overwrite it,
			// and do not release storage that another hook may have cloned from.
			(void)m_memory.release();
		}
	}

	CoopVTableHook(const CoopVTableHook&) = delete;
	CoopVTableHook& operator=(const CoopVTableHook&) = delete;
	CoopVTableHook(CoopVTableHook&&) = delete;
	CoopVTableHook& operator=(CoopVTableHook&&) = delete;

	[[nodiscard]] bool HookObject(std::uintptr_t objectAddress) noexcept
	{
		if (objectAddress == 0)
			return false;

		if (IsHooked() && !UnhookObject())
			return false;

		auto** vtableSlot = reinterpret_cast<std::uintptr_t**>(objectAddress);
		std::uintptr_t* originalVtable = *vtableSlot;
		if (!originalVtable)
			return false;

		auto memory = std::unique_ptr<std::uintptr_t[]>(
			new (std::nothrow) std::uintptr_t[kStorageEntryCount]());
		if (!memory)
			return false;

		std::memcpy(
			memory.get(),
			originalVtable - kDataBeforeStartEntries,
			kCopiedByteCount);

		m_objectVtableSlot = vtableSlot;
		m_originalVtable = originalVtable;
		m_memory = std::move(memory);
		*m_objectVtableSlot = m_memory.get() + kDataBeforeStartEntries;
		return true;
	}

	template <typename T>
	[[nodiscard]] bool HookObject(T* object) noexcept
	{
		return HookObject(reinterpret_cast<std::uintptr_t>(object));
	}

	[[nodiscard]] bool UnhookObject() noexcept
	{
		if (!IsHooked())
			return true;

		std::uintptr_t* clonedVtable = m_memory.get() + kDataBeforeStartEntries;
		if (*m_objectVtableSlot != clonedVtable)
			return false;

		*m_objectVtableSlot = m_originalVtable;
		m_memory.reset();
		m_objectVtableSlot = nullptr;
		m_originalVtable = nullptr;
		return true;
	}

	[[nodiscard]] bool IsHooked() const noexcept
	{
		return m_objectVtableSlot != nullptr &&
			m_originalVtable != nullptr &&
			m_memory != nullptr;
	}

	[[nodiscard]] std::uintptr_t HookMethod(
		std::size_t index,
		std::uintptr_t newMethod) noexcept
	{
		if (!IsHooked() || index >= kHookableMethodCount || newMethod == 0)
			return 0;

		const std::size_t memoryIndex = kDataBeforeStartEntries + index;
		const std::uintptr_t originalMethod = m_memory[memoryIndex];
		m_memory[memoryIndex] = newMethod;
		return originalMethod;
	}

	template <typename T>
	[[nodiscard]] T* HookMethod(std::size_t index, T* newMethod) noexcept
	{
		return reinterpret_cast<T*>(HookMethod(
			index,
			reinterpret_cast<std::uintptr_t>(newMethod)));
	}

	// Preserve Chairloader's existing mixed-unit geometry exactly: the prefix is
	// used as a uintptr_t entry count, while the memcpy extent is expressed in bytes.
	static constexpr std::size_t kDataBeforeStartEntries = 32;
	static constexpr std::size_t kVtableCopyBytes = 512;
	static constexpr std::size_t kCopiedByteCount =
		kDataBeforeStartEntries + kVtableCopyBytes;
	static constexpr std::size_t kStorageEntryCount =
		kDataBeforeStartEntries + kVtableCopyBytes;
	static constexpr std::size_t kCopiedEntryCount =
		kCopiedByteCount / sizeof(std::uintptr_t);
	static constexpr std::size_t kHookableMethodCount =
		kCopiedEntryCount - kDataBeforeStartEntries;

private:
	static_assert(kCopiedByteCount % sizeof(std::uintptr_t) == 0);
	static_assert(kCopiedEntryCount > kDataBeforeStartEntries);

	std::uintptr_t** m_objectVtableSlot = nullptr;
	std::uintptr_t* m_originalVtable = nullptr;
	std::unique_ptr<std::uintptr_t[]> m_memory;
};
