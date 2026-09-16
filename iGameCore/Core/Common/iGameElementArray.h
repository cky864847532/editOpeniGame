#ifndef iGameElementArray_h
#define iGameElementArray_h

#include "iGameObject.h"
#include "iGameVector.h"
#include <memory>
#include <type_traits>

IGAME_NAMESPACE_BEGIN
template<typename TElement>
class ElementArray : public Object, private std::vector<TElement> {
public:
    I_OBJECT(ElementArray);
    static Pointer New() { return new ElementArray; }
    using VectorType = std::vector<TElement>;
    using Iterator = TElement*;
    using ConstIterator = const TElement*;
    using Reference = typename VectorType::reference;
    using ConstReference = typename VectorType::const_reference;

    // Free all memory and initialize the array
    void Initialize() {
        ReleaseAdopted();
        std::vector<TElement> temp;
        this->VectorType::swap(temp);
    }

    // Reallocate memory, and the old memory is preserved. The array
    // size will not change. '_Newcapacity' is the number of elements.
    void Reserve(const IGsize _Newcapacity) {
        if (m_adoptedOwner) {
            if (_Newcapacity <= m_adoptedCapacity) { return; }
            VectorType expanded;
            expanded.reserve(_Newcapacity);
            if (m_adoptedSize != 0u) { expanded.assign(m_adoptedData, m_adoptedData + m_adoptedSize); }
            this->VectorType::swap(expanded);
            ReleaseAdopted();
            return;
        }
        this->VectorType::reserve(_Newcapacity);
    }

    // Reallocate memory, and the old memory is preserved. The array
    // size will change. '_Newsize' is the number of elements.
    void Resize(const IGsize _Newsize) { Resize(_Newsize, TElement{}); }
    void Resize(const IGsize _Newsize, const TElement& _Element) {
        if (m_adoptedOwner && _Newsize <= m_adoptedCapacity) {
            if (_Newsize > m_adoptedSize) { std::fill(m_adoptedData + m_adoptedSize, m_adoptedData + _Newsize, _Element); }
            m_adoptedSize = _Newsize;
            return;
        }
        Reserve(_Newsize);
        this->VectorType::resize(_Newsize, _Element);
    }

    // Reset the array size, and the old memory will not change.
    void Reset() { if (m_adoptedOwner) { m_adoptedSize = 0u; } else { this->VectorType::clear(); } }

    // 标量数组直接接管完成结果，扩容时迁移到可增长存储
    bool AdoptArray(std::shared_ptr<const void> owner, TElement* data, int dimension,
                    std::size_t size, std::size_t capacity) requires std::is_trivially_copyable_v<TElement> {
        if (!owner || dimension != 1 || size > capacity ||
            (capacity != 0u && (!data || reinterpret_cast<std::uintptr_t>(data) % alignof(TElement) != 0u))) { return false; }
        VectorType().swap(static_cast<VectorType&>(*this));
        m_adoptedOwner = std::move(owner);
        m_adoptedData = data;
        m_adoptedSize = size;
        m_adoptedCapacity = capacity;
        return true;
    }

    // Free up extra memory.
    void Squeeze() { this->Resize(Size()); }

    bool ShallowCopy(ElementArray<TElement>::Pointer other) { return false; }
    bool DeepCopy(ElementArray<TElement>::Pointer other) {
        if (other == nullptr) return false;
        this->Reserve(other->Size());
        for (int i = 0; i < other->Size(); i++) {
            this->AddElement(other->GetElement(i));
        }
        //this->Modified();
        return true;
    }

    // Add an element to the end of an array
    void AddElement(TElement&& _Element) {
        AddElement(static_cast<const TElement&>(_Element));
    }
    void AddElement(const TElement& _Element) {
        if (m_adoptedOwner) {
            const auto value = _Element;
            if (m_adoptedSize < m_adoptedCapacity) { m_adoptedData[m_adoptedSize++] = value; return; }
            Reserve(m_adoptedSize + 1u);
            this->VectorType::push_back(value);
            return;
        }
        this->VectorType::push_back(_Element);
    }

    // Return element's reference by index _Pos
    Reference ElementAt(const IGsize _Pos) {
        return this->RawPointer()[_Pos];
    }
    ConstReference ElementAt(const IGsize _Pos) const {
        return this->RawPointer()[_Pos];
    }

    // Get element by index _Pos
    Reference GetElement(const IGsize _Pos) {
        return this->RawPointer()[_Pos];
    }
    ConstReference GetElement(const IGsize _Pos) const {
        return this->RawPointer()[_Pos];
    }

    // Set element by index _Pos
    void SetElement(const IGsize _Pos, TElement&& _Element) {
        this->RawPointer()[_Pos] = _Element;
    }
    void SetElement(const IGsize _Pos, const TElement& _Element) {
        this->RawPointer()[_Pos] = _Element;
    }

    Iterator Remove(ConstIterator _Where) {
        const auto offset = _Where - Begin();
        if (m_adoptedOwner) {
            std::move(m_adoptedData + offset + 1u, m_adoptedData + m_adoptedSize, m_adoptedData + offset);
            --m_adoptedSize;
        } else { this->VectorType::erase(this->VectorType::cbegin() + offset); }
        return Begin() + offset;
    }

    Iterator Begin() { return RawPointer(); }
    ConstIterator Begin() const { return RawPointer(); }
    Iterator End() { return Size() == 0u ? Begin() : Begin() + Size(); }
    ConstIterator End() const { return Size() == 0u ? Begin() : Begin() + Size(); }

    IGsize GetNumberOfElements() const { return Size(); }

    IGsize Size() const { return m_adoptedOwner ? m_adoptedSize : this->VectorType::size(); }
    IGsize GetCapacity() const { return m_adoptedOwner ? m_adoptedCapacity : this->VectorType::capacity(); }

    TElement* RawPointer() { return m_adoptedOwner ? m_adoptedData : this->VectorType::data(); }
    const TElement* RawPointer() const { return m_adoptedOwner ? m_adoptedData : this->VectorType::data(); }

protected:
    void ReleaseAdopted() noexcept {
        m_adoptedOwner.reset();
        m_adoptedData = nullptr;
        m_adoptedSize = m_adoptedCapacity = 0u;
    }
    std::shared_ptr<const void> m_adoptedOwner;
    TElement* m_adoptedData{nullptr};
    std::size_t m_adoptedSize{0u};
    std::size_t m_adoptedCapacity{0u};
    ElementArray() {}
    ~ElementArray() override = default;
};
IGAME_NAMESPACE_END
#endif
