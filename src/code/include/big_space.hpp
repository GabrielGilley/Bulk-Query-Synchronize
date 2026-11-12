#pragma once

#include <exception>
#include <cstring>
#include <cstdio>
#include <random>
#include <memory>
#include <string>

#define CONFIG_BIGPIGO

#ifdef CONFIG_BIGPIGO
#define PIGO_SCRATCH "/scratch/"
#include "pigo.hpp"
#else
#include <sys/stat.h>
#endif

using namespace std;

namespace pando {

class BigSpace {
    private:
        char* space_;
        char* end_;
        char* ptr_;
        const size_t size_;

        #ifdef CONFIG_BIGPIGO
        pigo::WFile* file_;
        string fname_;
        string dname_;
        #endif

    public:
        BigSpace(size_t size=2ull*(1ull<<29)) :
            space_(nullptr), end_(nullptr), ptr_(nullptr), size_(size) 
            #ifdef CONFIG_BIGPIGO
            , file_(nullptr) 
            #endif
        {
            #ifdef CONFIG_BIGPIGO
            // Find an available file
            random_device rdev;
            mt19937 rng {rdev()};
            uniform_int_distribution<mt19937::result_type> rdist {0, 65535};

            while (true) {
                dname_ = PIGO_SCRATCH "bigspace." + to_string(rdist(rng));
                if (mkdir(dname_.c_str(), 0700) == 0)
                    break;
            }

            // Create a PIGO file with the desired size
            fname_ = dname_ + "/space";
            file_ = new pigo::WFile(fname_, size_);
            space_ = (char*)file_->fp();

            #else
            space_ = (char*)malloc(size_);
            if (space_ == nullptr)
                throw std::bad_alloc();
            memset(space_, 0xff, size_);
            #endif

            end_ = space_ + size_;
            ptr_ = space_;
        }
        ~BigSpace() {
            #ifdef CONFIG_BIGPIGO
            delete file_;
            // Now, delete the actual file itself
            remove(fname_.c_str());
            rmdir(dname_.c_str());
            #else
            free(space_);
            #endif
        }

        void* allocate(size_t size) {
            if (size == 0) size = 1;
            if (ptr_ > end_ || static_cast<size_t>(end_ - ptr_) < size) {
                cerr << "not enough space in big space allocation" << endl;
                throw std::bad_alloc();
            }
            char* res = ptr_;
            ptr_ += size;
            return static_cast<void*>(res);
        }

        void deallocate(void* ptr) {
            // For now, no memory cleanup
            (void)ptr;
        }

        void reset() {
            ptr_ = space_;
        }

        size_t used() const {
            return static_cast<size_t>(ptr_ - space_); 
        }

        size_t remaining() const {
            return static_cast<size_t>(end_ - ptr_);
        }

        size_t capacity() const {
            return size_;
        }

        char* begin() const {
            return space_;
        }

        char* current() const {
            return ptr_;
        }

        char* end() const {
            return end_;
        }

        BigSpace(const BigSpace& other) = delete;
        BigSpace(BigSpace&& other) noexcept = delete;
        BigSpace& operator=(const BigSpace& other) = delete;
        BigSpace& operator=(BigSpace&& other) noexcept = delete;
};

template <typename T>
class BigSpaceAllocator {
    private:
        std::shared_ptr<BigSpace> sp_;

        template <typename U>
        friend class BigSpaceAllocator;

    public:
        using value_type = T;
        using is_always_equal = std::false_type;

        BigSpaceAllocator() = delete;
        BigSpaceAllocator(std::shared_ptr<BigSpace> sp) : sp_(std::move(sp)) { }

        template <typename U>
        struct rebind {
            using other = BigSpaceAllocator<U>;
        };

        template <typename U>
        BigSpaceAllocator(const BigSpaceAllocator<U>& other) : sp_(other.sp_) { }

        template <typename U>
        BigSpaceAllocator& operator=(const BigSpaceAllocator<U>& other) {
            sp_ = other.sp_;
            return *this;
        }

        template <typename U>
        BigSpaceAllocator(BigSpaceAllocator<U>&& other) noexcept : sp_(std::move(other.sp_)) { } 

        template <typename U>
        BigSpaceAllocator& operator=(BigSpaceAllocator<U>&& other) noexcept {
            sp_ = std::move(other.sp_);
            return *this;
       }

        T* allocate(size_t n) {
            return static_cast<T*>(sp_->allocate(n * sizeof(T)));
        }

        void deallocate(T* data, size_t) {
            sp_->deallocate(data);
        }
        void deallocate(T* data) {
            sp_->deallocate(data);
        }

        bool operator!=(const BigSpaceAllocator<T>& other) const {
            return sp_ != other.sp_;
        }

        bool operator==(const BigSpaceAllocator<T>& other) const {
            return sp_ == other.sp_;
        } 
        
        size_t used_bytes() const {
            return sp_->used();
        }

        size_t remaining_bytes() const {
            return sp_->remaining();
        }

        size_t capacity_bytes() const {
            return sp_->capacity();
        }

        std::shared_ptr<BigSpace> space() const {
            return sp_;
        }
    };

}
