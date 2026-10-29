#pragma once
#include <array>
#include <cstddef>
#include <functional>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dinero::consensus::detail {
// Internal value storage for a forest version. Read access is const even when
// the wrapper is mutable: no reference can bypass a later version's detach.
// This provides value isolation, not synchronization of one mutable instance.
template<class T, size_t PageSize = 256>
class ForestPages {
    static_assert(PageSize != 0);
    static_assert(std::is_nothrow_default_constructible_v<T> && std::is_nothrow_move_assignable_v<T>);
    using Page = std::array<T, PageSize>;
    using Directory = std::vector<std::shared_ptr<Page>>;
    std::shared_ptr<Directory> pages_ = std::make_shared<Directory>();
    size_t size_ = 0;
    void DetachDirectory() {
        if (pages_.use_count() != 1) pages_ = std::make_shared<Directory>(*pages_);
    }
    Page& WritePage(size_t page) {
        DetachDirectory();
        auto& value = (*pages_)[page];
        if (value.use_count() != 1) value = std::make_shared<Page>(*value);
        return *value;
    }
public:
    size_t size() const noexcept { return size_; }
    const T& operator[](size_t index) const { return (*(*pages_)[index/PageSize])[index%PageSize]; }
    void Set(size_t index, T value) {
        if (index >= size_) throw std::out_of_range("forest page index");
        WritePage(index/PageSize)[index%PageSize] = std::move(value);
    }
    void resize(size_t next) {
        if (next == size_) return;
        const size_t count = next/PageSize + (next%PageSize != 0);
        DetachDirectory();
        if (next < size_) {
            // Clear the retained tail so a later growth cannot revive values.
            if (next%PageSize) {
                auto& page = WritePage(next/PageSize);
                for (size_t i=next%PageSize; i<PageSize; ++i) page[i] = T{};
            }
            pages_->resize(count);
        } else {
            // A failed allocation can leave unused zero pages, but publishes
            // neither a larger logical size nor a partially initialized cell.
            while (pages_->size() < count) pages_->push_back(std::make_shared<Page>());
        }
        size_ = next;
    }
    void push_back(const T& value) {
        if (size_ == size_t(-1)) throw std::length_error("forest page size");
        const size_t at = size_;
        resize(at+1);
        try { Set(at,value); }
        catch (...) { size_=at; throw; }
    }
    class const_iterator {
        const ForestPages* owner_ = nullptr;
        size_t at_ = 0;
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using reference = const T&;
        using pointer = const T*;
        const_iterator() = default;
        const_iterator(const ForestPages* owner,size_t at):owner_(owner),at_(at){}
        reference operator*() const {return (*owner_)[at_];}
        pointer operator->() const {return &**this;}
        const_iterator& operator++(){++at_;return *this;}
        const_iterator operator++(int){auto old=*this;++*this;return old;}
        friend bool operator==(const const_iterator& a,const const_iterator& b){return a.owner_==b.owner_&&a.at_==b.at_;}
        friend bool operator!=(const const_iterator& a,const const_iterator& b){return !(a==b);}
    };
    const_iterator begin() const {return {this,0};}
    const_iterator end() const {return {this,size_};}
};

// A State copy shares each hash partition; a write copies only its partition.
// This is not a worst-case constant-time promise: collisions can concentrate
// keys, and erasing by predicate still examines the whole current inventory.
template<class Key,class Value,class Hash=std::hash<Key>,size_t Shards=256>
class ForestMap {
    static_assert(Shards != 0);
    using Bucket = std::unordered_map<Key,Value,Hash>;
    std::array<std::shared_ptr<Bucket>,Shards> buckets_{};
    size_t size_ = 0;
    size_t Shard(const Key& key) const {return Hash{}(key)%Shards;}
    Bucket& WriteBucket(size_t shard) {
        auto& bucket=buckets_[shard];
        if (!bucket) bucket=std::make_shared<Bucket>();
        else if (bucket.use_count() != 1) bucket=std::make_shared<Bucket>(*bucket);
        return *bucket;
    }
public:
    class const_iterator {
        friend class ForestMap;
        const ForestMap* owner_ = nullptr;
        size_t shard_ = Shards;
        typename Bucket::const_iterator item_{};
        const_iterator(const ForestMap* owner,size_t shard,typename Bucket::const_iterator item)
            :owner_(owner),shard_(shard),item_(item){}
        void NextBucket() {
            while (shard_<Shards) {
                const auto& bucket=owner_->buckets_[shard_];
                if (bucket&&!bucket->empty()) {item_=bucket->cbegin();return;}
                ++shard_;
            }
        }
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = typename Bucket::value_type;
        using difference_type = std::ptrdiff_t;
        using reference = const value_type&;
        using pointer = const value_type*;
        const_iterator() = default;
        reference operator*() const {return *item_;}
        pointer operator->() const {return &*item_;}
        const_iterator& operator++() {
            ++item_;
            if(item_==owner_->buckets_[shard_]->cend()){++shard_;NextBucket();}
            return *this;
        }
        const_iterator operator++(int){auto old=*this;++*this;return old;}
        friend bool operator==(const const_iterator& a,const const_iterator& b) {
            return a.owner_==b.owner_&&a.shard_==b.shard_&&(a.shard_==Shards||a.item_==b.item_);
        }
        friend bool operator!=(const const_iterator& a,const const_iterator& b){return !(a==b);}
    };
    size_t size() const noexcept {return size_;}
    const_iterator begin() const {
        const_iterator it(this,0,{});it.NextBucket();return it;
    }
    const_iterator end() const {return {this,Shards,{}};}
    const_iterator find(const Key& key) const {
        const auto shard=Shard(key);const auto& bucket=buckets_[shard];
        if(!bucket)return end();const auto it=bucket->find(key);
        return it==bucket->end()?end():const_iterator(this,shard,it);
    }
    size_t count(const Key& key) const {return find(key)!=end();}
    std::pair<const_iterator,bool> emplace(const Key& key,const Value& value) {
        const auto found=find(key);if(found!=end())return {found,false};
        const auto shard=Shard(key);auto result=WriteBucket(shard).emplace(key,value);
        if(result.second)++size_;
        return {const_iterator(this,shard,result.first),result.second};
    }
    void Set(const Key& key,const Value& value) {
        const auto shard=Shard(key);auto& bucket=WriteBucket(shard);
        auto result=bucket.insert_or_assign(key,value);if(result.second)++size_;
    }
    size_t erase(const Key& key) {
        if(!count(key))return 0;
        const auto erased=WriteBucket(Shard(key)).erase(key);size_-=erased;return erased;
    }
    template<class Predicate> void EraseIf(Predicate predicate) {
        for(size_t shard=0;shard<Shards;++shard) {
            const auto& current=buckets_[shard];if(!current)continue;
            bool changes=false;
            for(const auto& item:*current)if(predicate(item.first)){changes=true;break;}
            if(!changes)continue;
            auto& bucket=WriteBucket(shard);
            for(auto it=bucket.begin();it!=bucket.end();) {
                if(predicate(it->first)){it=bucket.erase(it);--size_;}else ++it;
            }
        }
    }
};

template<class Key,class Hash=std::hash<Key>,size_t Shards=256>
class ForestSet {
    using Map=ForestMap<Key,unsigned char,Hash,Shards>;
    Map values_;
public:
    class const_iterator {
        typename Map::const_iterator it_;
    public:
        using iterator_category=std::forward_iterator_tag;
        using value_type=Key;
        using difference_type=std::ptrdiff_t;
        using reference=const Key&;
        using pointer=const Key*;
        const_iterator()=default;
        explicit const_iterator(typename Map::const_iterator it):it_(it){}
        reference operator*()const{return it_->first;}
        pointer operator->()const{return &it_->first;}
        const_iterator& operator++(){++it_;return *this;}
        const_iterator operator++(int){auto old=*this;++*this;return old;}
        friend bool operator==(const const_iterator&a,const const_iterator&b){return a.it_==b.it_;}
        friend bool operator!=(const const_iterator&a,const const_iterator&b){return !(a==b);}
    };
    size_t size() const noexcept{return values_.size();}
    size_t count(const Key& key) const{return values_.count(key);}
    void insert(const Key& key){values_.emplace(key,0);}
    size_t erase(const Key& key){return values_.erase(key);}
    template<class Predicate>void EraseIf(Predicate predicate){values_.EraseIf(predicate);}
    const_iterator begin()const{return const_iterator(values_.begin());}
    const_iterator end()const{return const_iterator(values_.end());}
};
} // namespace dinero::consensus::detail
