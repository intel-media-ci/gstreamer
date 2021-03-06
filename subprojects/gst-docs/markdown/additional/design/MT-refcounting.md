# Conventions for thread a safe API

The GStreamer API is designed to be thread safe. This means that API functions
can be called from multiple threads at the same time. GStreamer internally uses
threads to perform the data passing and various asynchronous services such as
the clock can also use threads.

This design decision has implications for the usage of the API and the objects
which this document explains.

## Multi-threading safety techniques

Several design patterns are used to guarantee object consistency in GStreamer.
This is an overview of the methods used in various GStreamer subsystems.

### Refcounting:

All shared objects have a refcount associated with them. Each reference
obtained to the object should increase the refcount and each reference lost
should decrease the refcount.

The refcounting is used to make sure that when another thread destroys the
object, the ones which still hold a reference to the object do not read from
invalid memory when accessing the object.

Refcounting is also used to ensure that mutable data structures are only
modified when they are owned by the calling code.

It is a requirement that when two threads have a handle on an object, the
refcount must be more than one. This means that when one thread passes an
object to another thread it must increase the refcount. This requirement makes
sure that one thread cannot suddenly dispose the object making the other
thread crash when it tries to access the pointer to invalid memory.

### Shared data structures and writability:

All objects have a refcount associated with them. Each reference obtained to
the object should increase the refcount and each reference lost should
decrease the refcount.

Each thread having a refcount to the object can safely read from the object.
but modifications made to the object should be preceded with a
`_get_writable()` function call. This function will check the refcount of the
object and if the object is referenced by more than one instance, a copy is
made of the object that is then by definition only referenced from the calling
thread. This new copy is then modifiable without being visible to other
refcount holders.

This technique is used for information objects that, once created, never
change their values. The lifetime of these objects is generally short, the
objects are usually simple and cheap to copy/create.

The advantage of this method is that no reader/writers locks are needed. all
threads can concurrently read but writes happen locally on a new copy. In most
cases `_get_writable()` can avoid a real copy because the calling method is the
only one holding a reference, which makes read/write very cheap.

The drawback is that sometimes 1 needless copy can be done. This would happen
when N threads call `_get_writable()` at the same time, all seeing that N
references are held on the object. In this case 1 copy too many will be done.
This is not a problem in any practical situation because the copy operation is
fast.

### Mutable substructures:

Special techniques are necessary to ensure the consistency of compound shared
objects. As mentioned above, shared objects need to have a reference count of
1 if they are to be modified. Implicit in this assumption is that all parts of
the shared object belong only to the object. For example, a `GstStructure` in
one `GstCaps` object should not belong to any other `GstCaps` object. This
condition suggests a parent-child relationship: structures can only be added
to parent object if they do not already have a parent object.

In addition, these substructures must not be modified while more than one code
segment has a reference on the parent object. For example, if the user creates
a `GstStructure`, adds it to a `GstCaps`, and the `GstCaps` is then referenced by
other code segments, the `GstStructure` should then become immutable, so that
changes to that data structure do not affect other parts of the code. This
means that the child is only mutable when the parent's reference count is 1,
as well as when the child structure has no parent.

The general solution to this problem is to include a field in child structures
pointing to the parent's atomic reference count. When set to NULL, this
indicates that the child has no parent. Otherwise, procedures that modify the
child structure must check if the parent's refcount is 1, and otherwise must
cause an error to be signaled.

Note that this is an internal implementation detail; application or plugin
code that calls `_get_writable()` on an object is guaranteed to receive an
object of refcount 1, which must then be writable. The only trick is that a
pointer to a child structure of an object is only valid while the calling code
has a reference on the parent object, because the parent is the owner of the
child.

### Object locking:

For objects that contain state information and generally have a longer
lifetime, object locking is used to update the information contained in the
object.

All readers and writers acquire the lock before accessing the object. Only one
thread is allowed access the protected structures at a time.

Object locking is used for all objects extending from `GstObject` such as
`GstElement`, `GstPad`.

Object locking can be done with recursive locks or regular mutexes. Object
locks in GStreamer are implemented with mutexes which cause deadlocks when
locked recursively from the same thread. This is done because regular mutexes
are cheaper.

### Atomic operations

Atomic operations are operations that are performed as one consistent
operation even when executed by multiple threads. They do however not use the
conventional aproach of using mutexes to protect the critical section but rely
on CPU features and instructions.

The advantages are mostly speed related since there are no heavyweight locks
involved. Most of these instructions also do not cause a context switch in case
of concurrent access but use a retry mechanism or spinlocking.

Disadvantages are that each of these instructions usually cause a cache flush
on multi-CPU machines when two processors perform concurrent access.

Atomic operations are generally used for refcounting and for the allocation of
small fixed size objects in a memchunk. They can also be used to implement a
lockfree list or stack.

### Compare and swap

As part of the atomic operations, compare-and-swap (CAS) can be used to access
or update a single property or pointer in an object without having to take a
lock.

This technique is currently not used in GStreamer but might be added in the
future in performance critical places.


## Objects

### Locking involved:

- atomic operations for refcounting
- object locking

All objects should have a lock associated with them. This lock is used to keep
internal consistency when multiple threads call API function on the object.

For objects that extend the GStreamer base object class this lock can be
obtained with the macros `GST_OBJECT_LOCK()` and `GST_OBJECT_UNLOCK()`. For other object that do
not extend from the base `GstObject` class these macros can be different.

### refcounting

All new objects created have the `FLOATING` flag set. This means that the object
is not owned or managed yet by anybody other than the one holding a reference
to the object. The object in this state has a reference count of 1.

Various object methods can take ownership of another object, this means that
after calling a method on object A with an object B as an argument, the object
B is made sole property of object A. This means that after the method call you
are not allowed to access the object anymore unless you keep an extra
reference to the object. An example of such a method is the `_bin_add()` method.
As soon as this function is called in a Bin, the element passed as an argument
is owned by the bin and you are not allowed to access it anymore without
taking a `_ref()` before adding it to the bin. The reason being that after the
`_bin_add()` call disposing the bin also destroys the element.

Taking ownership of an object happens through the process of "sinking" the
object. the `_sink()` method on an object will decrease the refcount of the
object if the FLOATING flag is set. The act of taking ownership of an object
is then performed as a `_ref()` followed by a `_sink()` call on the object.

The float/sink process is very useful when initializing elements that will
then be placed under control of a parent. The floating ref keeps the object
alive until it is parented, and once the object is parented you can forget
about it.

also see [relations](additional/design/relations.md)

### parent-child relations

One can create parent-child relationships with the `_object_set_parent()`
method. This method refs and sinks the object and assigns its parent property
to that of the managing parent.

The child is said to have a weak link to the parent since the refcount of the
parent is not increased in this process. This means that if the parent is
disposed it has to unset itself as the parent of the object before disposing
itself, else the child object holds a parent pointer to invalid memory.

The responsibilities for an object that sinks other objects are summarised as:

- taking ownership of the object
    - call `_object_set_parent()` to set itself as the object parent, this call
    will `_ref()` and `_sink()` the object.
    - keep reference to object in a datastructure such as a list or array.

- on dispose
    - call `_object_unparent()` to reset the parent property and unref the
    object.
    - remove the object from the list.

also see [relations](additional/design/relations.md)

### Properties

Most objects also expose state information with public properties in the
object. Two types of properties might exist: accessible with or without
holding the object lock. All properties should only be accessed with their
corresponding macros. The public object properties are marked in the .h files
with /*< public >*/. The public properties that require a lock to be held are
marked with `/*< public >*/` `/* with <lock_type> */`, where `<lock_type>` can
be `LOCK` or `STATE_LOCK` or any other lock to mark the type(s) of lock to be
held.

**Example**:

in `GstPad` there is a public property `direction`. It can be found in the
section marked as public and requiring the LOCK to be held. There exists
also a macro to access the property.

``` c
struct _GstRealPad {
  ...
  /*< public >*/ /* with LOCK */
  ...
  GstPadDirection                direction;
  ...
};

#define GST_RPAD_DIRECTION(pad)      (GST_REAL_PAD_CAST(pad)->direction)
```

Accessing the property is therefore allowed with the following code example:

``` c
GST_OBJECT_LOCK (pad);
direction = GST_RPAD_DIRECTION (pad);
GST_OBJECT_UNLOCK (pad);
```

### Property lifetime

All properties requiring a lock can change after releasing the associated
lock. This means that as long as you hold the lock, the state of the
object regarding the locked properties is consistent with the information
obtained. As soon as the lock is released, any values acquired from the
properties might not be valid anymore and can as best be described as a
snapshot of the state when the lock was held.

This means that all properties that require access beyond the scope of the
critial section should be copied or refcounted before releasing the lock.

Most object provide a `_get_<property>()` method to get a copy or refcounted
instance of the property value. The caller should not wory about any locks
but should unref/free the object after usage.

**Example**:

the following example correctly gets the peer pad of an element. It is
required to increase the refcount of the peer pad because as soon as the
lock is released, the peer could be unreffed and disposed, making the
pointer obtained in the critical section point to invalid memory.

``` c
GST_OBJECT_LOCK (pad);
peer = GST_RPAD_PEER (pad);
if (peer)
gst_object_ref (GST_OBJECT (peer));
GST_OBJECT_UNLOCK (pad);
... use peer ...

if (peer)
  gst_object_unref (GST_OBJECT (peer));
```

Note that after releasing the lock the peer might not actually be the peer
anymore of the pad. If you need to be sure it is, you need to extend the
critical section to include the operations on the peer.

The following code is equivalent to the above but with using the functions
to access object properties.

``` c
peer = gst_pad_get_peer (pad);
if (peer) {
  ... use peer ...

  gst_object_unref (GST_OBJECT (peer));
}
```

**Example**:

Accessing the name of an object makes a copy of the name. The caller of the
function should `g_free()` the name after usage.

``` c
GST_OBJECT_LOCK (object)
name = g_strdup (GST_OBJECT_NAME (object));
GST_OBJECT_UNLOCK (object)
... use name ...

g_free (name);
```

or:

``` c
name = gst_object_get_name (object);

... use name ...

g_free (name);
```

### Accessor methods

For aplications it is encouraged to use the public methods of the object. Most
useful operations can be performed with the methods so it is seldom required
to access the public fields manually.

All accessor methods that return an object should increase the refcount of the
returned object. The caller should `_unref()` the object after usage. Each
method should state this refcounting policy in the documentation.

### Accessing lists

If the object property is a list, concurrent list iteration is needed to get
the contents of the list. GStreamer uses the cookie mechanism to mark the last
update of a list. The list and the cookie are protected by the same lock. Each
update to a list requires the following actions:

- acquire lock
- update list
- update cookie
- release lock

Updating the cookie is usually done by incrementing its value by one. Since
cookies use guint32 its wraparound is for all practical reasons is not a
problem.

Iterating a list can safely be done by surrounding the list iteration with a
lock/unlock of the lock.

In some cases it is not a good idea to hold the lock for a long time while
iterating the list. The state change code for a bin in GStreamer, for example,
has to iterate over each element and perform a blocking call on each of them
potentially causing infinite bin locking. In this case the cookie can be used
to iterate a list.

**Example**:

The following algorithm iterates a list and reverses the updates in the
case a concurrent update was done to the list while iterating. The idea is
that whenever we reacquire the lock, we check for updates to the cookie to
decide if we are still iterating the right list.

```  c
GST_OBJECT_LOCK (lock);
/* grab list and cookie */
cookie = object->list_cookie;
list = object->list;
while (list) {
  GstObject *item = GST_OBJECT (list->data);
  /* need to ref the item before releasing the lock */
  gst_object_ref (item);
  GST_OBJECT_UNLOCK (lock);

  ... use/change item here...

  /* release item here */
  gst_object_unref (item);

  GST_OBJECT_LOCK (lock);
  if (cookie != object->list_cookie) {
    /* handle rollback caused by concurrent modification
     * of the list here */

    ...rollback changes to items...

    /* grab new cookie and list */
    cookie = object->list_cookie;
    list = object->list;
  }
  else {
    list = g_list_next (list);
  }
}
GST_OBJECT_UNLOCK (lock);
```

### GstIterator

`GstIterator` provides an easier way of retrieving elements in a concurrent
list. The following code example is equivalent to the previous example.

**Example**:

``` c
it = _get_iterator(object);
while (!done) {
    switch (gst_iterator_next (it, &item)) {
    case GST_ITERATOR_OK:

        ... use/change item here...

        /* release item here */
        gst_object_unref (item);
    break;
    case GST_ITERATOR_RESYNC:
        /* handle rollback caused by concurrent modification
    * of the list here */

    ...rollback changes to items...

    /* resync iterator to start again */
    gst_iterator_resync (it);
    break;
    case GST_ITERATOR_DONE:
    done = TRUE;
    break;
    }
}
gst_iterator_free (it);
```
