
//          Copyright Oliver Kowalke 2013.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_FIBERS_CONTEXT_H
#define BOOST_FIBERS_CONTEXT_H

#include <atomic>

#include <boost/assert.hpp>
#include <boost/config.hpp>
#include <boost/context/detail/invoke.hpp>
#include <boost/context/execution_context.hpp>
#include <boost/context/stack_context.hpp>
#include <boost/intrusive/list.hpp>
#include <boost/intrusive/parent_from_member.hpp>
#include <boost/intrusive_ptr.hpp>

#include <boost/fiber/detail/config.hpp>
#include <boost/fiber/fixedsize_stack.hpp>

#ifdef BOOST_HAS_ABI_HEADERS
#  include BOOST_ABI_PREFIX
#endif

namespace boost {
namespace fibers {

class context;
class fiber;
class scheduler;

namespace detail {

struct wait_tag;
typedef intrusive::list_member_hook<
    intrusive::tag< wait_tag >,
    intrusive::link_mode<
        intrusive::auto_unlink
    >
>                                 wait_hook;
// declaration of the functor that converts between
// the context class and the wait-hook
struct wait_functor {
    // required types
    typedef wait_hook               hook_type;
    typedef hook_type           *   hook_ptr;
    typedef const hook_type     *   const_hook_ptr;
    typedef context                 value_type;
    typedef value_type          *   pointer;
    typedef const value_type    *   const_pointer;

    // required static functions
    static hook_ptr to_hook_ptr( value_type &value);
    static const_hook_ptr to_hook_ptr( value_type const& value);
    static pointer to_value_ptr( hook_ptr n);
    static const_pointer to_value_ptr( const_hook_ptr n);
};

struct ready_tag;
typedef intrusive::list_member_hook<
    intrusive::tag< ready_tag >,
    intrusive::link_mode<
        intrusive::safe_link
    >
>                                       ready_hook;

struct terminated_tag;
typedef intrusive::list_member_hook<
    intrusive::tag< terminated_tag >,
    intrusive::link_mode<
        intrusive::auto_unlink
    >
>                                       terminated_hook;

}

struct main_context_t {};
constexpr main_context_t main_context = main_context_t();

struct dispatcher_context_t {};
constexpr dispatcher_context_t dispatcher_context = dispatcher_context_t();

struct worker_context_t {};
constexpr worker_context_t worker_context = worker_context_t();

class BOOST_FIBERS_DECL context {
public:
    detail::ready_hook      ready_hook_;
    detail::terminated_hook terminated_hook_;
    detail::wait_hook       wait_hook_;

    typedef intrusive::list<
        context,
        intrusive::function_hook< detail::wait_functor >,
        intrusive::constant_time_size< false > >   wait_queue_t;

private:
    enum flag_t {
        flag_main_context       = 1 << 1,
        flag_dispatcher_context = 1 << 2,
        flag_worker_context     = 1 << 3,
        flag_terminated         = 1 << 4
    };

    static thread_local context         *   active_;

#if ! defined(BOOST_FIBERS_NO_ATOMICS)
    std::atomic< std::size_t >              use_count_;
    std::atomic< int >                      flags_;
#else
    std::size_t                             use_count_;
    int                                     flags_;
#endif
    scheduler                           *   scheduler_;
    boost::context::execution_context       ctx_;
    wait_queue_t                            wait_queue_;

    void set_terminated_() noexcept;

    void  suspend_() noexcept;

protected:
    virtual void deallocate() {
    }

public:
    class id {
    private:
        context  *   impl_;

    public:
        id() noexcept :
            impl_( nullptr) {
        }

        explicit id( context * impl) noexcept :
            impl_( impl) {
        }

        bool operator==( id const& other) const noexcept {
            return impl_ == other.impl_;
        }

        bool operator!=( id const& other) const noexcept {
            return impl_ != other.impl_;
        }

        bool operator<( id const& other) const noexcept {
            return impl_ < other.impl_;
        }

        bool operator>( id const& other) const noexcept {
            return other.impl_ < impl_;
        }

        bool operator<=( id const& other) const noexcept {
            return ! ( * this > other);
        }

        bool operator>=( id const& other) const noexcept {
            return ! ( * this < other);
        }

        template< typename charT, class traitsT >
        friend std::basic_ostream< charT, traitsT > &
        operator<<( std::basic_ostream< charT, traitsT > & os, id const& other) {
            if ( nullptr != other.impl_) {
                return os << other.impl_;
            } else {
                return os << "{not-valid}";
            }
        }

        explicit operator bool() const noexcept {
            return nullptr != impl_;
        }

        bool operator!() const noexcept {
            return nullptr == impl_;
        }
    };

    static context * active() noexcept;

    static void active( context * active) noexcept;

    // main fiber context
    context( main_context_t);

    // dispatcher fiber context
    context( dispatcher_context_t, boost::context::preallocated const&,
         fixedsize_stack const&, scheduler *);

    // worker fiber context
    template< typename StackAlloc, typename Fn, typename ... Args >
    context( worker_context_t,
             boost::context::preallocated palloc, StackAlloc salloc,
             Fn && fn, Args && ... args) :
        ready_hook_(),
        terminated_hook_(),
        wait_hook_(),
        use_count_( 1), // fiber instance or scheduler owner
        flags_( flag_worker_context),
        scheduler_( nullptr),
        ctx_( palloc, salloc,
              // mutable: generated operator() is not const -> enables std::move( fn)
              // std::make_tuple: stores decayed copies of its args, implicitly unwraps std::reference_wrapper
              [=,fn=std::forward< Fn >( fn),tpl=std::make_tuple( std::forward< Args >( args) ...)] () mutable -> void {
                // invoke fiber function
                boost::context::detail::invoke_helper( std::move( fn), std::move( tpl) );
                // mark fiber as terminated
                set_terminated_();
                // notify waiting (joining) fibers
                release();
                // switch to another fiber
                suspend_();
                BOOST_ASSERT_MSG( false, "fiber already terminated");
              }),
        wait_queue_() {
    }

    virtual ~context();

    void set_scheduler( scheduler *);

    scheduler * get_scheduler() const noexcept;

    id get_id() const noexcept;

    void resume();

    void release() noexcept;

    void join() noexcept;

    bool is_main_context() const noexcept {
        return 0 != ( flags_ & flag_main_context);
    }

    bool is_dispatcher_context() const noexcept {
        return 0 != ( flags_ & flag_dispatcher_context);
    }

    bool is_worker_context() const noexcept {
        return 0 != ( flags_ & flag_worker_context);
    }

    bool is_terminated() const noexcept {
        return 0 != ( flags_ & flag_terminated);
    }

    bool wait_is_linked();

    bool ready_is_linked();

    void wait_unlink();

    friend void intrusive_ptr_add_ref( context * ctx) {
        BOOST_ASSERT( nullptr != ctx);
        ++ctx->use_count_;
    }

    friend void intrusive_ptr_release( context * ctx) {
        BOOST_ASSERT( nullptr != ctx);
        if ( 0 == --ctx->use_count_) {
            ctx->~context();
        }
    }
};

inline
static intrusive_ptr< context > make_dispatcher_context( scheduler * sched) {
    fixedsize_stack salloc; // use default satck-size
    boost::context::stack_context sctx = salloc.allocate();
#if defined(BOOST_NO_CXX14_CONSTEXPR) || defined(BOOST_NO_CXX11_STD_ALIGN)
    // reserve space for control structure
    std::size_t size = sctx.size - sizeof( context);
    void * sp = static_cast< char * >( sctx.sp) - sizeof( context);
#else
    constexpr std::size_t func_alignment = 64; // alignof( context);
    constexpr std::size_t func_size = sizeof( context);
    // reserve space on stack
    void * sp = static_cast< char * >( sctx.sp) - func_size - func_alignment;
    // align sp pointer
    std::size_t space = func_size + func_alignment;
    sp = std::align( func_alignment, func_size, sp, space);
    BOOST_ASSERT( nullptr != sp);
    // calculate remaining size
    std::size_t size = sctx.size - ( static_cast< char * >( sctx.sp) - static_cast< char * >( sp) );
#endif
    // placement new of context on top of fiber's stack
    return intrusive_ptr< context >( 
            new ( sp) context(
                dispatcher_context,
                boost::context::preallocated( sp, size, sctx),
                salloc,
                sched) );
}

template< typename StackAlloc, typename Fn, typename ... Args >
static intrusive_ptr< context > make_worker_context( StackAlloc salloc, Fn && fn, Args && ... args) {
    boost::context::stack_context sctx = salloc.allocate();
#if defined(BOOST_NO_CXX14_CONSTEXPR) || defined(BOOST_NO_CXX11_STD_ALIGN)
    // reserve space for control structure
    std::size_t size = sctx.size - sizeof( context);
    void * sp = static_cast< char * >( sctx.sp) - sizeof( context);
#else
    constexpr std::size_t func_alignment = 64; // alignof( context);
    constexpr std::size_t func_size = sizeof( context);
    // reserve space on stack
    void * sp = static_cast< char * >( sctx.sp) - func_size - func_alignment;
    // align sp pointer
    std::size_t space = func_size + func_alignment;
    sp = std::align( func_alignment, func_size, sp, space);
    BOOST_ASSERT( nullptr != sp);
    // calculate remaining size
    std::size_t size = sctx.size - ( static_cast< char * >( sctx.sp) - static_cast< char * >( sp) );
#endif
    // placement new of context on top of fiber's stack
    return intrusive_ptr< context >( 
            new ( sp) context(
                worker_context,
                boost::context::preallocated( sp, size, sctx),
                salloc,
                std::forward< Fn >( fn),
                std::forward< Args >( args) ... ) );
}

namespace detail {

inline
wait_functor::hook_ptr wait_functor::to_hook_ptr( wait_functor::value_type & value) {
    return & value.wait_hook_;
}

inline
wait_functor::const_hook_ptr wait_functor::to_hook_ptr( wait_functor::value_type const& value) {
    return & value.wait_hook_;
}

inline
wait_functor::pointer wait_functor::to_value_ptr( wait_functor::hook_ptr n) {
    return intrusive::get_parent_from_member< context >( n, & context::wait_hook_);
}

inline
wait_functor::const_pointer wait_functor::to_value_ptr( wait_functor::const_hook_ptr n) {
    return intrusive::get_parent_from_member< context >( n, & context::wait_hook_);
}

}}}

#ifdef BOOST_HAS_ABI_HEADERS
#  include BOOST_ABI_SUFFIX
#endif

#endif // BOOST_FIBERS_CONTEXT_H
