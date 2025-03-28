/*
 * Copyright (c) 1999, 2024, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#ifndef SHARE_PRIMS_JVMTIIMPL_HPP
#define SHARE_PRIMS_JVMTIIMPL_HPP

#include "jvmtifiles/jvmti.h"
#include "oops/objArrayOop.hpp"
#include "prims/jvmtiEnvThreadState.hpp"
#include "prims/jvmtiEventController.hpp"
#include "prims/jvmtiTrace.hpp"
#include "prims/jvmtiUtil.hpp"
#include "runtime/escapeBarrier.hpp"
#include "runtime/stackValueCollection.hpp"
#include "runtime/vmOperations.hpp"
#include "utilities/ostream.hpp"


///////////////////////////////////////////////////////////////
//
// class JvmtiBreakpoint
//
// A JvmtiBreakpoint describes a location (class, method, bci) to break at.
//

typedef void (Method::*method_action)(int _bci);

class JvmtiBreakpoint : public CHeapObj<mtInternal> {
private:
  Method*               _method;
  int                   _bci;
  OopHandle             _class_holder;  // keeps _method memory from being deallocated

public:
  JvmtiBreakpoint(Method* m_method, jlocation location);
  JvmtiBreakpoint(const JvmtiBreakpoint& bp);
  virtual ~JvmtiBreakpoint();
  bool equals(const JvmtiBreakpoint& bp) const;
  address getBcp() const;
  void each_method_version_do(method_action meth_act);
  void set();
  void clear();
  void print_on(outputStream* out) const;

  Method* method() const { return _method; }
};

///////////////////////////////////////////////////////////////
//
// class JvmtiBreakpoints
//
// Contains growable array of JvmtiBreakpoint.
// All changes to the array occur at a safepoint.
//

class JvmtiBreakpoints : public CHeapObj<mtInternal> {
private:
  GrowableArray<JvmtiBreakpoint*> _elements;

  int length() { return _elements.length(); }
  JvmtiBreakpoint& at(int index) { return *_elements.at(index); }
  int find(JvmtiBreakpoint& e) {
    return _elements.find_if([&](const JvmtiBreakpoint * other_e) { return e.equals(*other_e); });
  }
  void append(JvmtiBreakpoint& e) {
    JvmtiBreakpoint* new_e = new JvmtiBreakpoint(e);
    _elements.append(new_e);
  }
  void remove(int index) {
    JvmtiBreakpoint* e = _elements.at(index);
    assert(e != nullptr, "e != nullptr");
    _elements.remove_at(index);
    delete e;
  }

  friend class JvmtiCurrentBreakpoints;
  JvmtiBreakpoints(); // accessible only for JvmtiCurrentBreakpoints

public:
  ~JvmtiBreakpoints();

  void print();

  int  set(JvmtiBreakpoint& bp);
  int  clear(JvmtiBreakpoint& bp);

  // used by VM_ChangeBreakpoints
  void set_at_safepoint(JvmtiBreakpoint& bp);
  void clear_at_safepoint(JvmtiBreakpoint& bp);
  // used by VM_RedefineClasses
  void clearall_in_class_at_safepoint(Klass* klass);
};

///////////////////////////////////////////////////////////////
//
// class JvmtiCurrentBreakpoints
//
// A static wrapper class for the JvmtiBreakpoints that provides
// a function for lazily creating the JvmtiBreakpoints class.
//

class JvmtiCurrentBreakpoints : public AllStatic {
private:
  // Current breakpoints, lazily initialized by get_jvmti_breakpoints();
  static JvmtiBreakpoints *_jvmti_breakpoints;

public:
  // lazily create _jvmti_breakpoints
  static JvmtiBreakpoints& get_jvmti_breakpoints();
};

///////////////////////////////////////////////////////////////
//
// VM_ChangeBreakpoints implements a VM_Operation for ALL modifications to the JvmtiBreakpoints class.
//

class VM_ChangeBreakpoints : public VM_Operation {
private:
  JvmtiBreakpoints* _breakpoints;
  int               _operation;
  JvmtiBreakpoint*  _bp;

public:
  enum { SET_BREAKPOINT=0, CLEAR_BREAKPOINT=1 };

  VM_ChangeBreakpoints(int operation, JvmtiBreakpoint *bp) {
    JvmtiBreakpoints& current_bps = JvmtiCurrentBreakpoints::get_jvmti_breakpoints();
    _breakpoints = &current_bps;
    _bp = bp;
    _operation = operation;
    assert(bp != nullptr, "bp != null");
  }

  VMOp_Type type() const { return VMOp_ChangeBreakpoints; }
  void doit();
};


///////////////////////////////////////////////////////////////
// The get/set local operations must only be done by the VM thread
// because the interpreter version needs to access oop maps, which can
// only safely be done by the VM thread
//
// I'm told that in 1.5 oop maps are now protected by a lock and
// we could get rid of the VM op
// However if the VM op is removed then the target thread must
// be suspended AND a lock will be needed to prevent concurrent
// setting of locals to the same java thread. This lock is needed
// to prevent compiledVFrames from trying to add deferred updates
// to the thread simultaneously.
//
class VM_BaseGetOrSetLocal : public VM_Operation {
 protected:
  JavaThread* _calling_thread;
  jint        _depth;
  jint        _index;
  BasicType   _type;
  jvalue      _value;
  javaVFrame* _jvf;
  bool        _set;
  bool        _self;

  static const jvalue _DEFAULT_VALUE;

  // It is possible to get the receiver out of a non-static native wrapper
  // frame.  Use VM_GetReceiver to do this.
  virtual bool getting_receiver() const { return false; }

  jvmtiError  _result;

  virtual javaVFrame* get_java_vframe() = 0;
  bool check_slot_type_lvt(javaVFrame* vf);
  bool check_slot_type_no_lvt(javaVFrame* vf);

public:
  VM_BaseGetOrSetLocal(JavaThread* calling_thread, jint depth, jint index,
                       BasicType type, jvalue value, bool set, bool self);

  jvalue value()         { return _value; }
  jvmtiError result()    { return _result; }

  void doit();
  bool allow_nested_vm_operations() const;
  virtual const char* name() const = 0;

  // Check that the klass is assignable to a type with the given signature.
  static bool is_assignable(const char* ty_sign, Klass* klass, Thread* thread);
};


class VM_GetOrSetLocal : public VM_BaseGetOrSetLocal {
 protected:
  JavaThread* _thread;
  EscapeBarrier _eb;

  vframe* get_vframe();
  javaVFrame* get_java_vframe();

public:
  // Constructor for non-object getter
  VM_GetOrSetLocal(JavaThread* thread, jint depth, jint index, BasicType type, bool self);

  // Constructor for object or non-object setter
  VM_GetOrSetLocal(JavaThread* thread, jint depth, jint index, BasicType type, jvalue value, bool self);

  // Constructor for object getter
  VM_GetOrSetLocal(JavaThread* thread, JavaThread* calling_thread, jint depth, int index, bool self);

  VMOp_Type type() const { return VMOp_GetOrSetLocal; }

  bool doit_prologue();

  const char* name() const                       { return "get/set locals"; }
};

class VM_GetReceiver : public VM_GetOrSetLocal {
 protected:
  virtual bool getting_receiver() const { return true; }

 public:
  VM_GetReceiver(JavaThread* thread, JavaThread* calling_thread, jint depth, bool self);
  const char* name() const                       { return "get receiver"; }
};

// VM operation to get or set virtual thread local.
class VM_VirtualThreadGetOrSetLocal : public VM_BaseGetOrSetLocal {
 protected:
  JvmtiEnv *_env;
  Handle _vthread_h;

  javaVFrame* get_java_vframe();

public:
  // Constructor for non-object getter.
  VM_VirtualThreadGetOrSetLocal(JvmtiEnv* env, Handle vthread_h, jint depth, jint index, BasicType type, bool self);

  // Constructor for object or non-object setter.
  VM_VirtualThreadGetOrSetLocal(JvmtiEnv* env, Handle vthread_h, jint depth,
                                jint index, BasicType type, jvalue value, bool self);

  // Constructor for object getter.
  VM_VirtualThreadGetOrSetLocal(JvmtiEnv* env, Handle vthread_h, JavaThread* calling_thread,
                                jint depth, int index, bool self);

  VMOp_Type type() const { return VMOp_VirtualThreadGetOrSetLocal; }

  const char* name() const                       { return "virtual thread get/set locals"; }
};

class VM_VirtualThreadGetReceiver : public VM_VirtualThreadGetOrSetLocal {
 protected:
  virtual bool getting_receiver() const { return true; }

 public:
  VM_VirtualThreadGetReceiver(JvmtiEnv* env, Handle vthread_h, JavaThread* calling_thread, jint depth, bool self);
  const char* name() const                       { return "virtual thread get receiver"; }
};


/**
 * When a thread (such as the compiler thread or VM thread) cannot post a
 * JVMTI event itself because the event needs to be posted from a Java
 * thread, then it can defer the event to the Service thread for posting.
 * The information needed to post the event is encapsulated into this class
 * and then enqueued onto the JvmtiDeferredEventQueue, where the Service
 * thread will pick it up and post it.
 *
 * This is currently only used for posting compiled-method-load and unload
 * events, which we don't want posted from the compiler thread.
 */
class JvmtiDeferredEvent {
  friend class JvmtiDeferredEventQueue;
 private:
  typedef enum {
    TYPE_NONE,
    TYPE_COMPILED_METHOD_LOAD,
    TYPE_COMPILED_METHOD_UNLOAD,
    TYPE_DYNAMIC_CODE_GENERATED,
    TYPE_CLASS_UNLOAD
  } Type;

  Type _type;
  union {
    nmethod* compiled_method_load;
    struct {
      jmethodID method_id;
      const void* code_begin;
    } compiled_method_unload;
    struct {
      const char* name;
      const void* code_begin;
      const void* code_end;
    } dynamic_code_generated;
    struct {
      const char* name;
    } class_unload;
  } _event_data;

  JvmtiDeferredEvent(Type t) : _type(t) {}

 public:

  JvmtiDeferredEvent() : _type(TYPE_NONE) {}

  // Factory methods
  static JvmtiDeferredEvent compiled_method_load_event(nmethod* nm)
    NOT_JVMTI_RETURN_(JvmtiDeferredEvent());
  static JvmtiDeferredEvent compiled_method_unload_event(
      jmethodID id, const void* code) NOT_JVMTI_RETURN_(JvmtiDeferredEvent());
  static JvmtiDeferredEvent dynamic_code_generated_event(
      const char* name, const void* begin, const void* end)
          NOT_JVMTI_RETURN_(JvmtiDeferredEvent());
  static JvmtiDeferredEvent class_unload_event(
      const char* name) NOT_JVMTI_RETURN_(JvmtiDeferredEvent());

  // Actually posts the event.
  void post() NOT_JVMTI_RETURN;
  void post_compiled_method_load_event(JvmtiEnv* env) NOT_JVMTI_RETURN;
  void run_nmethod_entry_barriers() NOT_JVMTI_RETURN;
  // GC support to keep nmethods from unloading while in the queue.
  void nmethods_do(NMethodClosure* cf) NOT_JVMTI_RETURN;
  // GC support to keep nmethod from being unloaded while in the queue.
  void oops_do(OopClosure* f, NMethodClosure* cf) NOT_JVMTI_RETURN;
};

/**
 * Events enqueued on this queue wake up the Service thread which dequeues
 * and posts the events.  The Service_lock is required to be held
 * when operating on the queue.
 */
class JvmtiDeferredEventQueue : public CHeapObj<mtInternal> {
  friend class JvmtiDeferredEvent;
 private:
  class QueueNode : public CHeapObj<mtInternal> {
   private:
    JvmtiDeferredEvent _event;
    QueueNode* _next;

   public:
    QueueNode(const JvmtiDeferredEvent& event)
      : _event(event), _next(nullptr) {}

    JvmtiDeferredEvent& event() { return _event; }
    QueueNode* next() const { return _next; }

    void set_next(QueueNode* next) { _next = next; }
  };

  QueueNode* _queue_head;
  QueueNode* _queue_tail;

 public:
  JvmtiDeferredEventQueue() : _queue_head(nullptr), _queue_tail(nullptr) {}

  bool has_events() NOT_JVMTI_RETURN_(false);
  JvmtiDeferredEvent dequeue() NOT_JVMTI_RETURN_(JvmtiDeferredEvent());

  // Post all events in the queue for the current Jvmti environment
  void post(JvmtiEnv* env) NOT_JVMTI_RETURN;
  void enqueue(JvmtiDeferredEvent event) NOT_JVMTI_RETURN;
  void run_nmethod_entry_barriers();

  // GC support to keep nmethods from unloading while in the queue.
  void nmethods_do(NMethodClosure* cf) NOT_JVMTI_RETURN;
  // GC support to keep nmethod from being unloaded while in the queue.
  void oops_do(OopClosure* f, NMethodClosure* cf) NOT_JVMTI_RETURN;
};

// Utility macro that checks for null pointers:
#define NULL_CHECK(X, Y) if ((X) == nullptr) { return (Y); }

#endif // SHARE_PRIMS_JVMTIIMPL_HPP
