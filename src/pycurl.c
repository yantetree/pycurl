/* PycURL -- cURL Python module
 */

#include "pycurl.h"

/* Raise exception based on return value `res' and `self->error' */
#define CURLERROR_RETVAL() do {\
    PyObject *v; \
    self->error[sizeof(self->error) - 1] = 0; \
    v = Py_BuildValue("(is)", (int) (res), self->error); \
    if (v != NULL) { PyErr_SetObject(ErrorObject, v); Py_DECREF(v); } \
    return NULL; \
} while (0)

/* Raise exception based on return value `res' and custom message */
#define CURLERROR_MSG(msg) do {\
    PyObject *v; const char *m = (msg); \
    v = Py_BuildValue("(is)", (int) (res), (m)); \
    if (v != NULL) { PyErr_SetObject(ErrorObject, v); Py_DECREF(v); } \
    return NULL; \
} while (0)


/* Convert a curl slist (a list of strings) to a Python list.
 * In case of error return NULL with an exception set.
 */
static PyObject *convert_slist(struct curl_slist *slist, int free_flags)
{
    PyObject *ret = NULL;
    struct curl_slist *slist_start = slist;

    ret = PyList_New((Py_ssize_t)0);
    if (ret == NULL) goto error;

    for ( ; slist != NULL; slist = slist->next) {
        PyObject *v = NULL;

        if (slist->data == NULL) {
            v = Py_None; Py_INCREF(v);
        } else {
            v = PyText_FromString(slist->data);
        }
        if (v == NULL || PyList_Append(ret, v) != 0) {
            Py_XDECREF(v);
            goto error;
        }
        Py_DECREF(v);
    }

    if ((free_flags & 1) && slist_start)
        curl_slist_free_all(slist_start);
    return ret;

error:
    Py_XDECREF(ret);
    if ((free_flags & 2) && slist_start)
        curl_slist_free_all(slist_start);
    return NULL;
}

#ifdef HAVE_CURLOPT_CERTINFO
/* Convert a struct curl_certinfo into a Python data structure.
 * In case of error return NULL with an exception set.
 */
static PyObject *convert_certinfo(struct curl_certinfo *cinfo)
{
    PyObject *certs;
    int cert_index;

    if (!cinfo)
        Py_RETURN_NONE;

    certs = PyList_New((Py_ssize_t)(cinfo->num_of_certs));
    if (!certs)
        return NULL;

    for (cert_index = 0; cert_index < cinfo->num_of_certs; cert_index ++) {
        struct curl_slist *fields = cinfo->certinfo[cert_index];
        struct curl_slist *field_cursor;
        int field_count, field_index;
        PyObject *cert;

        field_count = 0;
        field_cursor = fields;
        while (field_cursor != NULL) {
            field_cursor = field_cursor->next;
            field_count ++;
        }

        
        cert = PyTuple_New((Py_ssize_t)field_count);
        if (!cert)
            goto error;
        PyList_SetItem(certs, cert_index, cert); /* Eats the ref from New() */
        
        for(field_index = 0, field_cursor = fields;
            field_cursor != NULL;
            field_index ++, field_cursor = field_cursor->next) {
            const char *field = field_cursor->data;
            PyObject *field_tuple;

            if (!field) {
                field_tuple = Py_None; Py_INCREF(field_tuple);
            } else {
                const char *sep = strchr(field, ':');
                if (!sep) {
                    field_tuple = PyText_FromString(field);
                } else {
                    /* XXX check */
                    field_tuple = Py_BuildValue("s#s", field, (int)(sep - field), sep+1);
                }
                if (!field_tuple)
                    goto error;
            }
            PyTuple_SET_ITEM(cert, field_index, field_tuple); /* Eats the ref */
        }
    }

    return certs;
    
 error:
    Py_DECREF(certs);
    return NULL;
}
#endif

/*************************************************************************
// static utility functions
**************************************************************************/

/* assert some CurlShareObject invariants */
static void
assert_share_state(const CurlShareObject *self)
{
    assert(self != NULL);
    assert(Py_TYPE(self) == p_CurlShare_Type);
#ifdef WITH_THREAD
    assert(self->lock != NULL);
#endif
}


/* assert some CurlObject invariants */
static void
assert_curl_state(const CurlObject *self)
{
    assert(self != NULL);
    assert(Py_TYPE(self) == p_Curl_Type);
#ifdef WITH_THREAD
    (void) pycurl_get_thread_state(self);
#endif
}


/* assert some CurlMultiObject invariants */
static void
assert_multi_state(const CurlMultiObject *self)
{
    assert(self != NULL);
    assert(Py_TYPE(self) == p_CurlMulti_Type);
#ifdef WITH_THREAD
    if (self->state != NULL) {
        assert(self->multi_handle != NULL);
    }
#endif
}


/* check state for methods */
static int
check_curl_state(const CurlObject *self, int flags, const char *name)
{
    assert_curl_state(self);
    if ((flags & 1) && self->handle == NULL) {
        PyErr_Format(ErrorObject, "cannot invoke %s() - no curl handle", name);
        return -1;
    }
#ifdef WITH_THREAD
    if ((flags & 2) && pycurl_get_thread_state(self) != NULL) {
        PyErr_Format(ErrorObject, "cannot invoke %s() - perform() is currently running", name);
        return -1;
    }
#endif
    return 0;
}

static int
check_multi_state(const CurlMultiObject *self, int flags, const char *name)
{
    assert_multi_state(self);
    if ((flags & 1) && self->multi_handle == NULL) {
        PyErr_Format(ErrorObject, "cannot invoke %s() - no multi handle", name);
        return -1;
    }
#ifdef WITH_THREAD
    if ((flags & 2) && self->state != NULL) {
        PyErr_Format(ErrorObject, "cannot invoke %s() - multi_perform() is currently running", name);
        return -1;
    }
#endif
    return 0;
}

static int
check_share_state(const CurlShareObject *self, int flags, const char *name)
{
    assert_share_state(self);
    return 0;
}

/* constructor - this is a module-level function returning a new instance */
PYCURL_INTERNAL CurlShareObject *
do_share_new(PyObject *dummy)
{
    int res;
    CurlShareObject *self;
#ifdef WITH_THREAD
    const curl_lock_function lock_cb = share_lock_callback;
    const curl_unlock_function unlock_cb = share_unlock_callback;
#endif

    UNUSED(dummy);

    /* Allocate python curl-share object */
    self = (CurlShareObject *) PyObject_GC_New(CurlShareObject, p_CurlShare_Type);
    if (self) {
        PyObject_GC_Track(self);
    }
    else {
        return NULL;
    }

    /* Initialize object attributes */
    self->dict = NULL;
#ifdef WITH_THREAD
    self->lock = share_lock_new();
    assert(self->lock != NULL);
#endif

    /* Allocate libcurl share handle */
    self->share_handle = curl_share_init();
    if (self->share_handle == NULL) {
        Py_DECREF(self);
        PyErr_SetString(ErrorObject, "initializing curl-share failed");
        return NULL;
    }

#ifdef WITH_THREAD
    /* Set locking functions and data  */
    res = curl_share_setopt(self->share_handle, CURLSHOPT_LOCKFUNC, lock_cb);
    assert(res == CURLE_OK);
    res = curl_share_setopt(self->share_handle, CURLSHOPT_USERDATA, self);
    assert(res == CURLE_OK);
    res = curl_share_setopt(self->share_handle, CURLSHOPT_UNLOCKFUNC, unlock_cb);
    assert(res == CURLE_OK);
#endif

    return self;
}


PYCURL_INTERNAL int
do_share_traverse(CurlShareObject *self, visitproc visit, void *arg)
{
    int err;
#undef VISIT
#define VISIT(v)    if ((v) != NULL && ((err = visit(v, arg)) != 0)) return err

    VISIT(self->dict);

    return 0;
#undef VISIT
}


/* Drop references that may have created reference cycles. */
PYCURL_INTERNAL int
do_share_clear(CurlShareObject *self)
{
    Py_CLEAR(self->dict);
    return 0;
}


static void
util_share_close(CurlShareObject *self){
    if (self->share_handle != NULL) {
        CURLSH *share_handle = self->share_handle;
        self->share_handle = NULL;
        curl_share_cleanup(share_handle);
    }
}


PYCURL_INTERNAL void
do_share_dealloc(CurlShareObject *self)
{
    PyObject_GC_UnTrack(self);
    Py_TRASHCAN_SAFE_BEGIN(self);

    Py_CLEAR(self->dict);
    util_share_close(self);

#ifdef WITH_THREAD
    share_lock_destroy(self->lock);
#endif

    PyObject_GC_Del(self);
    Py_TRASHCAN_SAFE_END(self)
}


static PyObject *
do_share_close(CurlShareObject *self)
{
    if (check_share_state(self, 2, "close") != 0) {
        return NULL;
    }
    util_share_close(self);
    Py_RETURN_NONE;
}


/* setopt, unsetopt*/
/* --------------- unsetopt/setopt/getinfo --------------- */

static PyObject *
do_curlshare_setopt(CurlShareObject *self, PyObject *args)
{
    int option;
    PyObject *obj;

    if (!PyArg_ParseTuple(args, "iO:setopt", &option, &obj))
        return NULL;
    if (check_share_state(self, 1 | 2, "sharesetopt") != 0)
        return NULL;

    /* early checks of option value */
    if (option <= 0)
        goto error;
    if (option >= (int)CURLOPTTYPE_OFF_T + OPTIONS_SIZE)
        goto error;
    if (option % 10000 >= OPTIONS_SIZE)
        goto error;

#if 0 /* XXX - should we ??? */
    /* Handle the case of None */
    if (obj == Py_None) {
        return util_curl_unsetopt(self, option);
    }
#endif

    /* Handle the case of integer arguments */
    if (PyInt_Check(obj)) {
        long d = PyInt_AsLong(obj);
        if (d != CURL_LOCK_DATA_COOKIE && d != CURL_LOCK_DATA_DNS && d != CURL_LOCK_DATA_SSL_SESSION) {
            goto error;
        }
        switch(option) {
        case CURLSHOPT_SHARE:
        case CURLSHOPT_UNSHARE:
            curl_share_setopt(self->share_handle, option, d);
            break;
        default:
            PyErr_SetString(PyExc_TypeError, "integers are not supported for this option");
            return NULL;
        }
        Py_RETURN_NONE;
    }
    /* Failed to match any of the function signatures -- return error */
error:
    PyErr_SetString(PyExc_TypeError, "invalid arguments to setopt");
    return NULL;
}


/*************************************************************************
// CurlObject
**************************************************************************/

/* --------------- construct/destruct (i.e. open/close) --------------- */

/* Allocate a new python curl object */
static CurlObject *
util_curl_new(void)
{
    CurlObject *self;

    self = (CurlObject *) PyObject_GC_New(CurlObject, p_Curl_Type);
    if (self == NULL)
        return NULL;
    PyObject_GC_Track(self);

    /* Set python curl object initial values */
    self->dict = NULL;
    self->handle = NULL;
#ifdef WITH_THREAD
    self->state = NULL;
#endif
    self->share = NULL;
    self->multi_stack = NULL;
    self->httppost = NULL;
    self->httppost_ref_list = NULL;
    self->httpheader = NULL;
    self->http200aliases = NULL;
    self->quote = NULL;
    self->postquote = NULL;
    self->prequote = NULL;
#ifdef HAVE_CURLOPT_RESOLVE
    self->resolve = NULL;
#endif

    /* Set callback pointers to NULL by default */
    self->w_cb = NULL;
    self->h_cb = NULL;
    self->r_cb = NULL;
    self->pro_cb = NULL;
    self->debug_cb = NULL;
    self->ioctl_cb = NULL;
    self->opensocket_cb = NULL;
    self->seek_cb = NULL;

    /* Set file object pointers to NULL by default */
    self->readdata_fp = NULL;
    self->writedata_fp = NULL;
    self->writeheader_fp = NULL;
    
    /* Set postfields object pointer to NULL by default */
    self->postfields_obj = NULL;

    /* Zero string pointer memory buffer used by setopt */
    memset(self->error, 0, sizeof(self->error));

    return self;
}

/* initializer - used to intialize curl easy handles for use with pycurl */
static int
util_curl_init(CurlObject *self)
{
    int res;

    /* Set curl error buffer and zero it */
    res = curl_easy_setopt(self->handle, CURLOPT_ERRORBUFFER, self->error);
    if (res != CURLE_OK) {
        return (-1);
    }
    memset(self->error, 0, sizeof(self->error));

    /* Set backreference */
    res = curl_easy_setopt(self->handle, CURLOPT_PRIVATE, (char *) self);
    if (res != CURLE_OK) {
        return (-1);
    }

    /* Enable NOPROGRESS by default, i.e. no progress output */
    res = curl_easy_setopt(self->handle, CURLOPT_NOPROGRESS, (long)1);
    if (res != CURLE_OK) {
        return (-1);
    }

    /* Disable VERBOSE by default, i.e. no verbose output */
    res = curl_easy_setopt(self->handle, CURLOPT_VERBOSE, (long)0);
    if (res != CURLE_OK) {
        return (-1);
    }

    /* Set FTP_ACCOUNT to NULL by default */
    res = curl_easy_setopt(self->handle, CURLOPT_FTP_ACCOUNT, NULL);
    if (res != CURLE_OK) {
        return (-1);
    }

    /* Set default USERAGENT */
    assert(g_pycurl_useragent);
    res = curl_easy_setopt(self->handle, CURLOPT_USERAGENT, g_pycurl_useragent);
    if (res != CURLE_OK) {
        return (-1);
    }
    return (0);
}

/* constructor - this is a module-level function returning a new instance */
PYCURL_INTERNAL CurlObject *
do_curl_new(PyObject *dummy)
{
    CurlObject *self = NULL;
    int res;

    UNUSED(dummy);

    /* Allocate python curl object */
    self = util_curl_new();
    if (self == NULL)
        return NULL;

    /* Initialize curl handle */
    self->handle = curl_easy_init();
    if (self->handle == NULL)
        goto error;

    res = util_curl_init(self);
    if (res < 0)
            goto error;
    /* Success - return new object */
    return self;

error:
    Py_DECREF(self);    /* this also closes self->handle */
    PyErr_SetString(ErrorObject, "initializing curl failed");
    return NULL;
}


/* util function shared by close() and clear() */
static void
util_curl_xdecref(CurlObject *self, int flags, CURL *handle)
{
    if (flags & PYCURL_MEMGROUP_ATTRDICT) {
        /* Decrement refcount for attributes dictionary. */
        Py_CLEAR(self->dict);
    }

    if (flags & PYCURL_MEMGROUP_MULTI) {
        /* Decrement refcount for multi_stack. */
        if (self->multi_stack != NULL) {
            CurlMultiObject *multi_stack = self->multi_stack;
            self->multi_stack = NULL;
            if (multi_stack->multi_handle != NULL && handle != NULL) {
                (void) curl_multi_remove_handle(multi_stack->multi_handle, handle);
            }
            Py_DECREF(multi_stack);
        }
    }

    if (flags & PYCURL_MEMGROUP_CALLBACK) {
        /* Decrement refcount for python callbacks. */
        Py_CLEAR(self->w_cb);
        Py_CLEAR(self->h_cb);
        Py_CLEAR(self->r_cb);
        Py_CLEAR(self->pro_cb);
        Py_CLEAR(self->debug_cb);
        Py_CLEAR(self->ioctl_cb);
        Py_CLEAR(self->opensocket_cb);
        Py_CLEAR(self->seek_cb);
    }

    if (flags & PYCURL_MEMGROUP_FILE) {
        /* Decrement refcount for python file objects. */
        Py_CLEAR(self->readdata_fp);
        Py_CLEAR(self->writedata_fp);
        Py_CLEAR(self->writeheader_fp);
    }

    if (flags & PYCURL_MEMGROUP_POSTFIELDS) {
        /* Decrement refcount for postfields object */
        Py_CLEAR(self->postfields_obj);
    }
    
    if (flags & PYCURL_MEMGROUP_SHARE) {
        /* Decrement refcount for share objects. */
        if (self->share != NULL) {
            CurlShareObject *share = self->share;
            self->share = NULL;
            if (share->share_handle != NULL && handle != NULL) {
                curl_easy_setopt(handle, CURLOPT_SHARE, NULL);
            }
            Py_DECREF(share);
        }
    }

    if (flags & PYCURL_MEMGROUP_HTTPPOST) {
        /* Decrement refcounts for httppost related references. */
        Py_CLEAR(self->httppost_ref_list);
    }
}


static void
util_curl_close(CurlObject *self)
{
    CURL *handle;

    /* Zero handle and thread-state to disallow any operations to be run
     * from now on */
    assert(self != NULL);
    assert(Py_TYPE(self) == p_Curl_Type);
    handle = self->handle;
    self->handle = NULL;
    if (handle == NULL) {
        /* Some paranoia assertions just to make sure the object
         * deallocation problem is finally really fixed... */
#ifdef WITH_THREAD
        assert(self->state == NULL);
#endif
        assert(self->multi_stack == NULL);
        assert(self->share == NULL);
        return;             /* already closed */
    }
#ifdef WITH_THREAD
    self->state = NULL;
#endif

    /* Decref multi stuff which uses this handle */
    util_curl_xdecref(self, PYCURL_MEMGROUP_MULTI, handle);
    /* Decref share which uses this handle */
    util_curl_xdecref(self, PYCURL_MEMGROUP_SHARE, handle);

    /* Cleanup curl handle - must be done without the gil */
    Py_BEGIN_ALLOW_THREADS
    curl_easy_cleanup(handle);
    Py_END_ALLOW_THREADS
    handle = NULL;

    /* Decref easy related objects */
    util_curl_xdecref(self, PYCURL_MEMGROUP_EASY, handle);

    /* Free all variables allocated by setopt */
#undef SFREE
#define SFREE(v)   if ((v) != NULL) (curl_formfree(v), (v) = NULL)
    SFREE(self->httppost);
#undef SFREE
#define SFREE(v)   if ((v) != NULL) (curl_slist_free_all(v), (v) = NULL)
    SFREE(self->httpheader);
    SFREE(self->http200aliases);
    SFREE(self->quote);
    SFREE(self->postquote);
    SFREE(self->prequote);
#ifdef HAVE_CURLOPT_RESOLVE
    SFREE(self->resolve);
#endif
#undef SFREE
}


PYCURL_INTERNAL void
do_curl_dealloc(CurlObject *self)
{
    PyObject_GC_UnTrack(self);
    Py_TRASHCAN_SAFE_BEGIN(self)

    Py_CLEAR(self->dict);
    util_curl_close(self);

    PyObject_GC_Del(self);
    Py_TRASHCAN_SAFE_END(self)
}


static PyObject *
do_curl_close(CurlObject *self)
{
    if (check_curl_state(self, 2, "close") != 0) {
        return NULL;
    }
    util_curl_close(self);
    Py_RETURN_NONE;
}


static PyObject *
do_curl_errstr(CurlObject *self)
{
    if (check_curl_state(self, 1 | 2, "errstr") != 0) {
        return NULL;
    }
    self->error[sizeof(self->error) - 1] = 0;

    return PyText_FromString(self->error);
}


/* --------------- GC support --------------- */

/* Drop references that may have created reference cycles. */
PYCURL_INTERNAL int
do_curl_clear(CurlObject *self)
{
#ifdef WITH_THREAD
    assert(pycurl_get_thread_state(self) == NULL);
#endif
    util_curl_xdecref(self, PYCURL_MEMGROUP_ALL, self->handle);
    return 0;
}

/* Traverse all refcounted objects. */
PYCURL_INTERNAL int
do_curl_traverse(CurlObject *self, visitproc visit, void *arg)
{
    int err;
#undef VISIT
#define VISIT(v)    if ((v) != NULL && ((err = visit(v, arg)) != 0)) return err

    VISIT(self->dict);
    VISIT((PyObject *) self->multi_stack);
    VISIT((PyObject *) self->share);

    VISIT(self->w_cb);
    VISIT(self->h_cb);
    VISIT(self->r_cb);
    VISIT(self->pro_cb);
    VISIT(self->debug_cb);
    VISIT(self->ioctl_cb);
    VISIT(self->opensocket_cb);
    VISIT(self->seek_cb);

    VISIT(self->readdata_fp);
    VISIT(self->writedata_fp);
    VISIT(self->writeheader_fp);
    
    VISIT(self->postfields_obj);

    return 0;
#undef VISIT
}


/* --------------- perform --------------- */

static PyObject *
do_curl_perform(CurlObject *self)
{
    int res;

    if (check_curl_state(self, 1 | 2, "perform") != 0) {
        return NULL;
    }

    PYCURL_BEGIN_ALLOW_THREADS
    res = curl_easy_perform(self->handle);
    PYCURL_END_ALLOW_THREADS

    if (res != CURLE_OK) {
        CURLERROR_RETVAL();
    }
    Py_RETURN_NONE;
}


/* --------------- callback handlers --------------- */

/* IMPORTANT NOTE: due to threading issues, we cannot call _any_ Python
 * function without acquiring the thread state in the callback handlers.
 */

static size_t
util_write_callback(int flags, char *ptr, size_t size, size_t nmemb, void *stream)
{
    CurlObject *self;
    PyObject *arglist;
    PyObject *result = NULL;
    size_t ret = 0;     /* assume error */
    PyObject *cb;
    int total_size;
    PYCURL_DECLARE_THREAD_STATE;

    /* acquire thread */
    self = (CurlObject *)stream;
    if (!PYCURL_ACQUIRE_THREAD())
        return ret;

    /* check args */
    cb = flags ? self->h_cb : self->w_cb;
    if (cb == NULL)
        goto silent_error;
    if (size <= 0 || nmemb <= 0)
        goto done;
    total_size = (int)(size * nmemb);
    if (total_size < 0 || (size_t)total_size / size != nmemb) {
        PyErr_SetString(ErrorObject, "integer overflow in write callback");
        goto verbose_error;
    }

    /* run callback */
#if PY_MAJOR_VERSION >= 3
    arglist = Py_BuildValue("(y#)", ptr, total_size);
#else
    arglist = Py_BuildValue("(s#)", ptr, total_size);
#endif
    if (arglist == NULL)
        goto verbose_error;
    result = PyEval_CallObject(cb, arglist);
    Py_DECREF(arglist);
    if (result == NULL)
        goto verbose_error;

    /* handle result */
    if (result == Py_None) {
        ret = total_size;           /* None means success */
    }
    else if (PyInt_Check(result) || PyLong_Check(result)) {
        /* if the cast to long fails, PyLong_AsLong() returns -1L */
        ret = (size_t) PyLong_AsLong(result);
    }
    else {
        PyErr_SetString(ErrorObject, "write callback must return int or None");
        goto verbose_error;
    }

done:
silent_error:
    Py_XDECREF(result);
    PYCURL_RELEASE_THREAD();
    return ret;
verbose_error:
    PyErr_Print();
    goto silent_error;
}


static size_t
write_callback(char *ptr, size_t size, size_t nmemb, void *stream)
{
    return util_write_callback(0, ptr, size, nmemb, stream);
}

static size_t
header_callback(char *ptr, size_t size, size_t nmemb, void *stream)
{
    return util_write_callback(1, ptr, size, nmemb, stream);
}

/* convert protocol address from C to python, returns a tuple of protocol
   specific values */
static PyObject *
convert_protocol_address(struct sockaddr* saddr, unsigned int saddrlen)
{
    PyObject *res_obj = NULL;
    
    switch (saddr->sa_family)
    {
    case AF_INET:
        {
            struct sockaddr_in* sin = (struct sockaddr_in*)saddr;
            char *addr_str = (char *)PyMem_Malloc(INET_ADDRSTRLEN);
            
            if (addr_str == NULL) {
                PyErr_SetString(ErrorObject, "Out of memory");
                goto error;
            }
            
            if (inet_ntop(saddr->sa_family, &sin->sin_addr, addr_str, INET_ADDRSTRLEN) == NULL) {
                PyErr_SetFromErrno(ErrorObject);
                PyMem_Free(addr_str);
                goto error;
            }
            res_obj = Py_BuildValue("(si)", addr_str, ntohs(sin->sin_port));
            PyMem_Free(addr_str);
       }
        break;
    case AF_INET6:
        {
            struct sockaddr_in6* sin6 = (struct sockaddr_in6*)saddr;
            char *addr_str = (char *)PyMem_Malloc(INET6_ADDRSTRLEN);
            
            if (addr_str == NULL) {
                PyErr_SetString(ErrorObject, "Out of memory");
                goto error;
            }
            
            if (inet_ntop(saddr->sa_family, &sin6->sin6_addr, addr_str, INET6_ADDRSTRLEN) == NULL) {
                PyErr_SetFromErrno(ErrorObject);
                PyMem_Free(addr_str);
                goto error;
            }
            res_obj = Py_BuildValue("(si)", addr_str, ntohs(sin6->sin6_port));
            PyMem_Free(addr_str);
        }
        break;
    default:
        /* We (currently) only support IPv4/6 addresses.  Can curl even be used
           with anything else? */
        PyErr_SetString(ErrorObject, "Unsupported address family.");
    }
    
error:
    return res_obj;
}

/* curl_socket_t is just an int on unix/windows (with limitations that
 * are not important here) */
static curl_socket_t
opensocket_callback(void *clientp, curlsocktype purpose,
                    struct curl_sockaddr *address)
{
    PyObject *arglist;
    PyObject *result = NULL;
    PyObject *fileno_result = NULL;
    CurlObject *self;
    int ret = CURL_SOCKET_BAD;
    PYCURL_DECLARE_THREAD_STATE;

    self = (CurlObject *)clientp;
    PYCURL_ACQUIRE_THREAD();
    
    arglist = Py_BuildValue("(iiiN)", address->family, address->socktype, address->protocol, convert_protocol_address(&address->addr, address->addrlen));
    if (arglist == NULL)
        goto verbose_error;

    result = PyEval_CallObject(self->opensocket_cb, arglist);

    Py_DECREF(arglist);
    if (result == NULL) {
        goto verbose_error;
    }

    if (PyObject_HasAttrString(result, "fileno")) {
        fileno_result = PyObject_CallMethod(result, "fileno", NULL);

        if (fileno_result == NULL) {
            ret = CURL_SOCKET_BAD;
            goto verbose_error;
        }
        // normal operation:
        if (PyInt_Check(fileno_result)) {
            int sockfd = PyInt_AsLong(fileno_result);
#if defined(WIN32)
            ret = dup_winsock(sockfd, address);
#else
            ret = dup(sockfd);
#endif
            goto done;
        }
    } else {
        PyErr_SetString(ErrorObject, "Return value must be a socket.");
        ret = CURL_SOCKET_BAD;
        goto verbose_error;
    }

silent_error:
done:
    Py_XDECREF(result);
    Py_XDECREF(fileno_result);
    PYCURL_RELEASE_THREAD();
    return ret;
verbose_error:
    PyErr_Print();
    goto silent_error;
}

static int
seek_callback(void *stream, curl_off_t offset, int origin)
{
    CurlObject *self;
    PyObject *arglist;
    PyObject *result = NULL;
    int ret = 2;     /* assume error 2 (can't seek, libcurl free to work around). */
    PyObject *cb;
    int source = 0;     /* assume beginning */
    PYCURL_DECLARE_THREAD_STATE;

    /* acquire thread */
    self = (CurlObject *)stream;
    if (!PYCURL_ACQUIRE_THREAD())
        return ret;

    /* check arguments */
    switch (origin)
    {
      case SEEK_SET:
          source = 0;
          break;
      case SEEK_CUR:
          source = 1;
          break;
      case SEEK_END:
          source = 2;
          break;
      default:
          source = origin;
          break;
    }
    
    /* run callback */
    cb = self->seek_cb;
    if (cb == NULL)
        goto silent_error;
    arglist = Py_BuildValue("(i,i)", offset, source);
    if (arglist == NULL)
        goto verbose_error;
    result = PyEval_CallObject(cb, arglist);
    Py_DECREF(arglist);
    if (result == NULL)
        goto verbose_error;

    /* handle result */
    if (result == Py_None) {
        ret = 0;           /* None means success */
    }
    else if (PyInt_Check(result)) {
        int ret_code = PyInt_AsLong(result);
        if (ret_code < 0 || ret_code > 2) {
            PyErr_Format(ErrorObject, "invalid return value for seek callback %d not in (0, 1, 2)", ret_code);
            goto verbose_error;
        }
        ret = ret_code;    /* pass the return code from the callback */
    }
    else {
        PyErr_SetString(ErrorObject, "seek callback must return 0 (CURL_SEEKFUNC_OK), 1 (CURL_SEEKFUNC_FAIL), 2 (CURL_SEEKFUNC_CANTSEEK) or None");
        goto verbose_error;
    }

silent_error:
    Py_XDECREF(result);
    PYCURL_RELEASE_THREAD();
    return ret;
verbose_error:
    PyErr_Print();
    goto silent_error;
}




static size_t
read_callback(char *ptr, size_t size, size_t nmemb, void *stream)
{
    CurlObject *self;
    PyObject *arglist;
    PyObject *result = NULL;

    size_t ret = CURL_READFUNC_ABORT;     /* assume error, this actually works */
    int total_size;

    PYCURL_DECLARE_THREAD_STATE;
    
    /* acquire thread */
    self = (CurlObject *)stream;
    if (!PYCURL_ACQUIRE_THREAD())
        return ret;

    /* check args */
    if (self->r_cb == NULL)
        goto silent_error;
    if (size <= 0 || nmemb <= 0)
        goto done;
    total_size = (int)(size * nmemb);
    if (total_size < 0 || (size_t)total_size / size != nmemb) {
        PyErr_SetString(ErrorObject, "integer overflow in read callback");
        goto verbose_error;
    }

    /* run callback */
    arglist = Py_BuildValue("(i)", total_size);
    if (arglist == NULL)
        goto verbose_error;
    result = PyEval_CallObject(self->r_cb, arglist);
    Py_DECREF(arglist);
    if (result == NULL)
        goto verbose_error;

    /* handle result */
    if (PyByteStr_Check(result)) {
        char *buf = NULL;
        Py_ssize_t obj_size = -1;
        Py_ssize_t r;
        r = PyByteStr_AsStringAndSize(result, &buf, &obj_size);
        if (r != 0 || obj_size < 0 || obj_size > total_size) {
            PyErr_Format(ErrorObject, "invalid return value for read callback (%ld bytes returned when at most %ld bytes were wanted)", (long)obj_size, (long)total_size);
            goto verbose_error;
        }
        memcpy(ptr, buf, obj_size);
        ret = obj_size;             /* success */
    }
    else if (PyUnicode_Check(result)) {
        char *buf = NULL;
        Py_ssize_t obj_size = -1;
        Py_ssize_t r;
        /*
        Encode with ascii codec.
        
        HTTP requires sending content-length for request body to the server
        before the request body is sent, therefore typically content length
        is given via POSTFIELDSIZE before read function is invoked to
        provide the data.
        
        However, if we encode the string using any encoding other than ascii,
        the length of encoded string may not match the length of unicode
        string we are encoding. Therefore, if client code does a simple
        len(source_string) to determine the value to supply in content-length,
        the length of bytes read may be different.
        
        To avoid this situation, we only accept ascii bytes in the string here.
        
        Encode data yourself to bytes when dealing with non-ascii data.
        */
        PyObject *encoded = PyUnicode_AsEncodedString(result, "ascii", "strict");
        if (encoded == NULL) {
            goto verbose_error;
        }
        r = PyByteStr_AsStringAndSize(encoded, &buf, &obj_size);
        if (r != 0 || obj_size < 0 || obj_size > total_size) {
            Py_DECREF(encoded);
            PyErr_Format(ErrorObject, "invalid return value for read callback (%ld bytes returned after encoding to utf-8 when at most %ld bytes were wanted)", (long)obj_size, (long)total_size);
            goto verbose_error;
        }
        memcpy(ptr, buf, obj_size);
        Py_DECREF(encoded);
        ret = obj_size;             /* success */
    }
#if PY_MAJOR_VERSION < 3
    else if (PyInt_Check(result)) {
        long r = PyInt_AsLong(result);
        if (r != CURL_READFUNC_ABORT && r != CURL_READFUNC_PAUSE)
            goto type_error;
        ret = r; /* either CURL_READFUNC_ABORT or CURL_READFUNC_PAUSE */
    }
#endif
    else if (PyLong_Check(result)) {
        long r = PyLong_AsLong(result);
        if (r != CURL_READFUNC_ABORT && r != CURL_READFUNC_PAUSE)
            goto type_error;
        ret = r; /* either CURL_READFUNC_ABORT or CURL_READFUNC_PAUSE */
    }
    else {
    type_error:
        PyErr_SetString(ErrorObject, "read callback must return a byte string or Unicode string with ASCII code points only");
        goto verbose_error;
    }
    
done:
silent_error:
    Py_XDECREF(result);
    PYCURL_RELEASE_THREAD();
    return ret;
verbose_error:
    PyErr_Print();
    goto silent_error;
}


static int
progress_callback(void *stream,
                  double dltotal, double dlnow, double ultotal, double ulnow)
{
    CurlObject *self;
    PyObject *arglist;
    PyObject *result = NULL;
    int ret = 1;       /* assume error */
    PYCURL_DECLARE_THREAD_STATE;

    /* acquire thread */
    self = (CurlObject *)stream;
    if (!PYCURL_ACQUIRE_THREAD())
        return ret;

    /* check args */
    if (self->pro_cb == NULL)
        goto silent_error;

    /* run callback */
    arglist = Py_BuildValue("(dddd)", dltotal, dlnow, ultotal, ulnow);
    if (arglist == NULL)
        goto verbose_error;
    result = PyEval_CallObject(self->pro_cb, arglist);
    Py_DECREF(arglist);
    if (result == NULL)
        goto verbose_error;

    /* handle result */
    if (result == Py_None) {
        ret = 0;        /* None means success */
    }
    else if (PyInt_Check(result)) {
        ret = (int) PyInt_AsLong(result);
    }
    else {
        ret = PyObject_IsTrue(result);  /* FIXME ??? */
    }

silent_error:
    Py_XDECREF(result);
    PYCURL_RELEASE_THREAD();
    return ret;
verbose_error:
    PyErr_Print();
    goto silent_error;
}


static int
debug_callback(CURL *curlobj, curl_infotype type,
               char *buffer, size_t total_size, void *stream)
{
    CurlObject *self;
    PyObject *arglist;
    PyObject *result = NULL;
    int ret = 0;       /* always success */
    PYCURL_DECLARE_THREAD_STATE;

    UNUSED(curlobj);

    /* acquire thread */
    self = (CurlObject *)stream;
    if (!PYCURL_ACQUIRE_THREAD())
        return ret;

    /* check args */
    if (self->debug_cb == NULL)
        goto silent_error;
    if ((int)total_size < 0 || (size_t)((int)total_size) != total_size) {
        PyErr_SetString(ErrorObject, "integer overflow in debug callback");
        goto verbose_error;
    }

    /* run callback */
    arglist = Py_BuildValue("(is#)", (int)type, buffer, (int)total_size);
    if (arglist == NULL)
        goto verbose_error;
    result = PyEval_CallObject(self->debug_cb, arglist);
    Py_DECREF(arglist);
    if (result == NULL)
        goto verbose_error;

    /* return values from debug callbacks should be ignored */

silent_error:
    Py_XDECREF(result);
    PYCURL_RELEASE_THREAD();
    return ret;
verbose_error:
    PyErr_Print();
    goto silent_error;
}


static curlioerr
ioctl_callback(CURL *curlobj, int cmd, void *stream)
{
    CurlObject *self;
    PyObject *arglist;
    PyObject *result = NULL;
    int ret = CURLIOE_FAILRESTART;       /* assume error */
    PYCURL_DECLARE_THREAD_STATE;

    UNUSED(curlobj);

    /* acquire thread */
    self = (CurlObject *)stream;
    if (!PYCURL_ACQUIRE_THREAD())
        return (curlioerr) ret;

    /* check args */
    if (self->ioctl_cb == NULL)
        goto silent_error;

    /* run callback */
    arglist = Py_BuildValue("(i)", cmd);
    if (arglist == NULL)
        goto verbose_error;
    result = PyEval_CallObject(self->ioctl_cb, arglist);
    Py_DECREF(arglist);
    if (result == NULL)
        goto verbose_error;

    /* handle result */
    if (result == Py_None) {
        ret = CURLIOE_OK;        /* None means success */
    }
    else if (PyInt_Check(result)) {
        ret = (int) PyInt_AsLong(result);
        if (ret >= CURLIOE_LAST || ret < 0) {
            PyErr_SetString(ErrorObject, "ioctl callback returned invalid value");
            goto verbose_error;
        }
    }

silent_error:
    Py_XDECREF(result);
    PYCURL_RELEASE_THREAD();
    return (curlioerr) ret;
verbose_error:
    PyErr_Print();
    goto silent_error;
}


/* ------------------------ reset ------------------------ */

static PyObject*
do_curl_reset(CurlObject *self)
{
    int res;

    curl_easy_reset(self->handle);

    /* Decref easy interface related objects */
    util_curl_xdecref(self, PYCURL_MEMGROUP_EASY, self->handle);

    /* Free all variables allocated by setopt */
#undef SFREE
#define SFREE(v)   if ((v) != NULL) (curl_formfree(v), (v) = NULL)
    SFREE(self->httppost);
#undef SFREE
#define SFREE(v)   if ((v) != NULL) (curl_slist_free_all(v), (v) = NULL)
    SFREE(self->httpheader);
    SFREE(self->http200aliases);
    SFREE(self->quote);
    SFREE(self->postquote);
    SFREE(self->prequote);
#ifdef HAVE_CURLOPT_RESOLVE
    SFREE(self->resolve);
#endif
#undef SFREE
    res = util_curl_init(self);
    if (res < 0) {
        Py_DECREF(self);    /* this also closes self->handle */
        PyErr_SetString(ErrorObject, "resetting curl failed");
        return NULL;
    }

    Py_RETURN_NONE;
}

/* --------------- unsetopt/setopt/getinfo --------------- */

static PyObject *
util_curl_unsetopt(CurlObject *self, int option)
{
    int res;

#define SETOPT2(o,x) \
    if ((res = curl_easy_setopt(self->handle, (o), (x))) != CURLE_OK) goto error
#define SETOPT(x)   SETOPT2((CURLoption)option, (x))

    /* FIXME: implement more options. Have to carefully check lib/url.c in the
     *   libcurl source code to see if it's actually safe to simply
     *   unset the option. */
    switch (option)
    {
    case CURLOPT_SHARE:
        SETOPT((CURLSH *) NULL);
        Py_XDECREF(self->share);
        self->share = NULL;
        break;
    case CURLOPT_HTTPPOST:
        SETOPT((void *) 0);
        curl_formfree(self->httppost);
        util_curl_xdecref(self, PYCURL_MEMGROUP_HTTPPOST, self->handle);
        self->httppost = NULL;
        /* FIXME: what about data->set.httpreq ?? */
        break;
    case CURLOPT_INFILESIZE:
        SETOPT((long) -1);
        break;
    case CURLOPT_WRITEHEADER:
        SETOPT((void *) 0);
        Py_CLEAR(self->writeheader_fp);
        break;
    case CURLOPT_CAINFO:
    case CURLOPT_CAPATH:
    case CURLOPT_COOKIE:
    case CURLOPT_COOKIEJAR:
    case CURLOPT_CUSTOMREQUEST:
    case CURLOPT_EGDSOCKET:
    case CURLOPT_FTPPORT:
    case CURLOPT_PROXYUSERPWD:
#ifdef HAVE_CURLOPT_PROXYUSERNAME
    case CURLOPT_PROXYUSERNAME:
    case CURLOPT_PROXYPASSWORD:
#endif
    case CURLOPT_RANDOM_FILE:
    case CURLOPT_SSL_CIPHER_LIST:
    case CURLOPT_USERPWD:
#ifdef HAVE_CURLOPT_USERNAME
    case CURLOPT_USERNAME:
    case CURLOPT_PASSWORD:
#endif
    case CURLOPT_RANGE:
        SETOPT((char *) 0);
        break;

#ifdef HAVE_CURLOPT_CERTINFO
    case CURLOPT_CERTINFO:
        SETOPT((long) 0);
        break;
#endif

    /* info: we explicitly list unsupported options here */
    case CURLOPT_COOKIEFILE:
    default:
        PyErr_SetString(PyExc_TypeError, "unsetopt() is not supported for this option");
        return NULL;
    }

    Py_RETURN_NONE;

error:
    CURLERROR_RETVAL();

#undef SETOPT
#undef SETOPT2
}


static PyObject *
do_curl_unsetopt(CurlObject *self, PyObject *args)
{
    int option;

    if (!PyArg_ParseTuple(args, "i:unsetopt", &option)) {
        return NULL;
    }
    if (check_curl_state(self, 1 | 2, "unsetopt") != 0) {
        return NULL;
    }

    /* early checks of option value */
    if (option <= 0)
        goto error;
    if (option >= (int)CURLOPTTYPE_OFF_T + OPTIONS_SIZE)
        goto error;
    if (option % 10000 >= OPTIONS_SIZE)
        goto error;

    return util_curl_unsetopt(self, option);

error:
    PyErr_SetString(PyExc_TypeError, "invalid arguments to unsetopt");
    return NULL;
}


static PyObject *
do_curl_setopt(CurlObject *self, PyObject *args)
{
    int option;
    PyObject *obj;
    int res;
    PyObject *encoded_obj;

    if (!PyArg_ParseTuple(args, "iO:setopt", &option, &obj))
        return NULL;
    if (check_curl_state(self, 1 | 2, "setopt") != 0)
        return NULL;

    /* early checks of option value */
    if (option <= 0)
        goto error;
    if (option >= (int)CURLOPTTYPE_OFF_T + OPTIONS_SIZE)
        goto error;
    if (option % 10000 >= OPTIONS_SIZE)
        goto error;

    /* Handle the case of None as the call of unsetopt() */
    if (obj == Py_None) {
        return util_curl_unsetopt(self, option);
    }

    /* Handle the case of string arguments */

    if (PyText_Check(obj)) {
        char *str = NULL;
        Py_ssize_t len = -1;

        /* Check that the option specified a string as well as the input */
        switch (option) {
        case CURLOPT_CAINFO:
        case CURLOPT_CAPATH:
        case CURLOPT_COOKIE:
        case CURLOPT_COOKIEFILE:
        case CURLOPT_COOKIELIST:
        case CURLOPT_COOKIEJAR:
        case CURLOPT_CUSTOMREQUEST:
        case CURLOPT_EGDSOCKET:
        case CURLOPT_ENCODING:
        case CURLOPT_FTPPORT:
        case CURLOPT_INTERFACE:
        case CURLOPT_KRB4LEVEL:
        case CURLOPT_NETRC_FILE:
        case CURLOPT_PROXY:
        case CURLOPT_PROXYUSERPWD:
#ifdef HAVE_CURLOPT_PROXYUSERNAME
        case CURLOPT_PROXYUSERNAME:
        case CURLOPT_PROXYPASSWORD:
#endif
        case CURLOPT_RANDOM_FILE:
        case CURLOPT_RANGE:
        case CURLOPT_REFERER:
        case CURLOPT_SSLCERT:
        case CURLOPT_SSLCERTTYPE:
        case CURLOPT_SSLENGINE:
        case CURLOPT_SSLKEY:
        case CURLOPT_SSLKEYPASSWD:
        case CURLOPT_SSLKEYTYPE:
        case CURLOPT_SSL_CIPHER_LIST:
        case CURLOPT_URL:
        case CURLOPT_USERAGENT:
        case CURLOPT_USERPWD:
#ifdef HAVE_CURLOPT_USERNAME
        case CURLOPT_USERNAME:
        case CURLOPT_PASSWORD:
#endif
        case CURLOPT_FTP_ALTERNATIVE_TO_USER:
        case CURLOPT_SSH_PUBLIC_KEYFILE:
        case CURLOPT_SSH_PRIVATE_KEYFILE:
        case CURLOPT_COPYPOSTFIELDS:
        case CURLOPT_SSH_HOST_PUBLIC_KEY_MD5:
        case CURLOPT_CRLFILE:
        case CURLOPT_ISSUERCERT:
#ifdef HAVE_CURLOPT_DNS_SERVERS
        case CURLOPT_DNS_SERVERS:
#endif
#ifdef HAVE_CURLOPT_NOPROXY
        case CURLOPT_NOPROXY:
#endif
/* FIXME: check if more of these options allow binary data */
            str = PyText_AsString_NoNUL(obj, &encoded_obj);
            if (str == NULL)
                return NULL;
            break;
        case CURLOPT_POSTFIELDS:
            if (PyText_AsStringAndSize(obj, &str, &len, &encoded_obj) != 0)
                return NULL;
            /* automatically set POSTFIELDSIZE */
            if (len <= INT_MAX) {
                res = curl_easy_setopt(self->handle, CURLOPT_POSTFIELDSIZE, (long)len);
            } else {
                res = curl_easy_setopt(self->handle, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)len);
            }
            if (res != CURLE_OK) {
                CURLERROR_RETVAL();
            }
            break;
        default:
            PyErr_SetString(PyExc_TypeError, "strings are not supported for this option");
            return NULL;
        }
        assert(str != NULL);
        /* Call setopt */
        res = curl_easy_setopt(self->handle, (CURLoption)option, str);
        /* Check for errors */
        if (res != CURLE_OK) {
            PyText_EncodedDecref(encoded_obj);
            CURLERROR_RETVAL();
        }
        /* libcurl does not copy the value of CURLOPT_POSTFIELDS */
        if (option == CURLOPT_POSTFIELDS) {
            PyObject *store_obj;
#if PY_MAJOR_VERSION >= 3
            /* if obj was bytes, it was not encoded, and we need to incref obj.
             * if obj was unicode, it was encoded, and we need to incref
             * encoded_obj - except we can simply transfer ownership.
             */
            if (encoded_obj) {
                store_obj = encoded_obj;
            } else {
                store_obj = obj;
                Py_INCREF(store_obj);
            }
#else
            /* no encoding is performed, incref the original object. */
            store_obj = obj;
            Py_INCREF(store_obj);
#endif
            util_curl_xdecref(self, PYCURL_MEMGROUP_POSTFIELDS, self->handle);
            self->postfields_obj = store_obj;
        } else {
            PyText_EncodedDecref(encoded_obj);
        }
        Py_RETURN_NONE;
    }

#define IS_LONG_OPTION(o)   (o < CURLOPTTYPE_OBJECTPOINT)
#define IS_OFF_T_OPTION(o)  (o >= CURLOPTTYPE_OFF_T)

    /* Handle the case of integer arguments */
    if (PyInt_Check(obj)) {
        long d = PyInt_AsLong(obj);

        if (IS_LONG_OPTION(option))
            res = curl_easy_setopt(self->handle, (CURLoption)option, (long)d);
        else if (IS_OFF_T_OPTION(option))
            res = curl_easy_setopt(self->handle, (CURLoption)option, (curl_off_t)d);
        else {
            PyErr_SetString(PyExc_TypeError, "integers are not supported for this option");
            return NULL;
        }
        if (res != CURLE_OK) {
            CURLERROR_RETVAL();
        }
        Py_RETURN_NONE;
    }

    /* Handle the case of long arguments (used by *_LARGE options) */
    if (PyLong_Check(obj)) {
        PY_LONG_LONG d = PyLong_AsLongLong(obj);
        if (d == -1 && PyErr_Occurred())
            return NULL;

        if (IS_LONG_OPTION(option) && (long)d == d)
            res = curl_easy_setopt(self->handle, (CURLoption)option, (long)d);
        else if (IS_OFF_T_OPTION(option) && (curl_off_t)d == d)
            res = curl_easy_setopt(self->handle, (CURLoption)option, (curl_off_t)d);
        else {
            PyErr_SetString(PyExc_TypeError, "longs are not supported for this option");
            return NULL;
        }
        if (res != CURLE_OK) {
            CURLERROR_RETVAL();
        }
        Py_RETURN_NONE;
    }

#undef IS_LONG_OPTION
#undef IS_OFF_T_OPTION

#if PY_MAJOR_VERSION < 3 && !defined(PYCURL_AVOID_STDIO)
    /* Handle the case of file objects */
    if (PyFile_Check(obj)) {
        FILE *fp;

        /* Ensure the option specified a file as well as the input */
        switch (option) {
        case CURLOPT_READDATA:
        case CURLOPT_WRITEDATA:
            break;
        case CURLOPT_WRITEHEADER:
            if (self->w_cb != NULL) {
                PyErr_SetString(ErrorObject, "cannot combine WRITEHEADER with WRITEFUNCTION.");
                return NULL;
            }
            break;
        default:
            PyErr_SetString(PyExc_TypeError, "files are not supported for this option");
            return NULL;
        }

        fp = PyFile_AsFile(obj);
        if (fp == NULL) {
            PyErr_SetString(PyExc_TypeError, "second argument must be open file");
            return NULL;
        }
        res = curl_easy_setopt(self->handle, (CURLoption)option, fp);
        if (res != CURLE_OK) {
            CURLERROR_RETVAL();
        }
        Py_INCREF(obj);

        switch (option) {
        case CURLOPT_READDATA:
            Py_CLEAR(self->readdata_fp);
            self->readdata_fp = obj;
            break;
        case CURLOPT_WRITEDATA:
            Py_CLEAR(self->writedata_fp);
            self->writedata_fp = obj;
            break;
        case CURLOPT_WRITEHEADER:
            Py_CLEAR(self->writeheader_fp);
            self->writeheader_fp = obj;
            break;
        default:
            assert(0);
            break;
        }
        /* Return success */
        Py_RETURN_NONE;
    }
#endif

    /* Handle the case of list objects */
    if (PyList_Check(obj)) {
        struct curl_slist **old_slist = NULL;
        struct curl_slist *slist = NULL;
        Py_ssize_t i, len;

        switch (option) {
        case CURLOPT_HTTP200ALIASES:
            old_slist = &self->http200aliases;
            break;
        case CURLOPT_HTTPHEADER:
            old_slist = &self->httpheader;
            break;
        case CURLOPT_QUOTE:
            old_slist = &self->quote;
            break;
        case CURLOPT_POSTQUOTE:
            old_slist = &self->postquote;
            break;
        case CURLOPT_PREQUOTE:
            old_slist = &self->prequote;
            break;
#ifdef HAVE_CURLOPT_RESOLVE
        case CURLOPT_RESOLVE:
            old_slist = &self->resolve;
            break;
#endif
        case CURLOPT_HTTPPOST:
            break;
        default:
            /* None of the list options were recognized, raise exception */
            PyErr_SetString(PyExc_TypeError, "lists are not supported for this option");
            return NULL;
        }

        len = PyList_Size(obj);
        if (len == 0)
            Py_RETURN_NONE;

        /* Handle HTTPPOST different since we construct a HttpPost form struct */
        if (option == CURLOPT_HTTPPOST) {
            struct curl_httppost *post = NULL;
            struct curl_httppost *last = NULL;
            /* List of all references that have been INCed as a result of
             * this operation */
            PyObject *ref_params = NULL;
            PyObject *nencoded_obj, *cencoded_obj, *oencoded_obj;

            for (i = 0; i < len; i++) {
                char *nstr = NULL, *cstr = NULL;
                Py_ssize_t nlen = -1, clen = -1;
                PyObject *listitem = PyList_GetItem(obj, i);

                if (!PyTuple_Check(listitem)) {
                    curl_formfree(post);
                    Py_XDECREF(ref_params);
                    PyErr_SetString(PyExc_TypeError, "list items must be tuple objects");
                    return NULL;
                }
                if (PyTuple_GET_SIZE(listitem) != 2) {
                    curl_formfree(post);
                    Py_XDECREF(ref_params);
                    PyErr_SetString(PyExc_TypeError, "tuple must contain two elements (name, value)");
                    return NULL;
                }
                if (PyText_AsStringAndSize(PyTuple_GET_ITEM(listitem, 0), &nstr, &nlen, &nencoded_obj) != 0) {
                    curl_formfree(post);
                    Py_XDECREF(ref_params);
                    PyErr_SetString(PyExc_TypeError, "tuple must contain a byte string or Unicode string with ASCII code points only as first element");
                    return NULL;
                }
                if (PyText_Check(PyTuple_GET_ITEM(listitem, 1))) {
                    /* Handle strings as second argument for backwards compatibility */

                    if (PyText_AsStringAndSize(PyTuple_GET_ITEM(listitem, 1), &cstr, &clen, &cencoded_obj)) {
                        curl_formfree(post);
                        Py_XDECREF(ref_params);
                        CURLERROR_RETVAL();
                    }
                    /* INFO: curl_formadd() internally does memdup() the data, so
                     * embedded NUL characters _are_ allowed here. */
                    res = curl_formadd(&post, &last,
                                       CURLFORM_COPYNAME, nstr,
                                       CURLFORM_NAMELENGTH, (long) nlen,
                                       CURLFORM_COPYCONTENTS, cstr,
                                       CURLFORM_CONTENTSLENGTH, (long) clen,
                                       CURLFORM_END);
                    PyText_EncodedDecref(cencoded_obj);
                    if (res != CURLE_OK) {
                        curl_formfree(post);
                        Py_XDECREF(ref_params);
                        CURLERROR_RETVAL();
                    }
                }
                else if (PyTuple_Check(PyTuple_GET_ITEM(listitem, 1))) {
                    /* Supports content, file and content-type */
                    PyObject *t = PyTuple_GET_ITEM(listitem, 1);
                    Py_ssize_t tlen = PyTuple_Size(t);
                    int j, k, l;
                    struct curl_forms *forms = NULL;

                    /* Sanity check that there are at least two tuple items */
                    if (tlen < 2) {
                        curl_formfree(post);
                        Py_XDECREF(ref_params);
                        PyErr_SetString(PyExc_TypeError, "tuple must contain at least one option and one value");
                        return NULL;
                    }

                    /* Allocate enough space to accommodate length options for content or buffers, plus a terminator. */
                    forms = PyMem_Malloc(sizeof(struct curl_forms) * ((tlen*2) + 1));
                    if (forms == NULL) {
                        curl_formfree(post);
                        Py_XDECREF(ref_params);
                        PyErr_NoMemory();
                        return NULL;
                    }

                    /* Iterate all the tuple members pairwise */
                    for (j = 0, k = 0, l = 0; j < tlen; j += 2, l++) {
                        char *ostr;
                        Py_ssize_t olen;
                        int val;

                        if (j == (tlen-1)) {
                            PyErr_SetString(PyExc_TypeError, "expected value");
                            PyMem_Free(forms);
                            curl_formfree(post);
                            Py_XDECREF(ref_params);
                            return NULL;
                        }
                        if (!PyInt_Check(PyTuple_GET_ITEM(t, j))) {
                            PyErr_SetString(PyExc_TypeError, "option must be long");
                            PyMem_Free(forms);
                            curl_formfree(post);
                            Py_XDECREF(ref_params);
                            return NULL;
                        }
                        if (!PyText_Check(PyTuple_GET_ITEM(t, j+1))) {
                            PyErr_SetString(PyExc_TypeError, "value must be a byte string or a Unicode string with ASCII code points only");
                            PyMem_Free(forms);
                            curl_formfree(post);
                            Py_XDECREF(ref_params);
                            return NULL;
                        }

                        val = PyLong_AsLong(PyTuple_GET_ITEM(t, j));
                        if (val != CURLFORM_COPYCONTENTS &&
                            val != CURLFORM_FILE &&
                            val != CURLFORM_FILENAME &&
                            val != CURLFORM_CONTENTTYPE &&
                            val != CURLFORM_BUFFER &&
                            val != CURLFORM_BUFFERPTR)
                        {
                            PyErr_SetString(PyExc_TypeError, "unsupported option");
                            PyMem_Free(forms);
                            curl_formfree(post);
                            Py_XDECREF(ref_params);
                            return NULL;
                        }
                        if (PyText_AsStringAndSize(PyTuple_GET_ITEM(t, j+1), &ostr, &olen, &oencoded_obj)) {
                            /* exception should be already set */
                            PyMem_Free(forms);
                            curl_formfree(post);
                            Py_XDECREF(ref_params);
                            return NULL;
                        }
                        forms[k].option = val;
                        forms[k].value = ostr;
                        ++k;
                        if (val == CURLFORM_COPYCONTENTS) {
                            /* Contents can contain \0 bytes so we specify the length */
                            forms[k].option = CURLFORM_CONTENTSLENGTH;
                            forms[k].value = (const char *)olen;
                            ++k;
                        }
                        else if (val == CURLFORM_BUFFERPTR) {
                            PyObject *obj = PyTuple_GET_ITEM(t, j+1);

                            ref_params = PyList_New((Py_ssize_t)0);
                            if (ref_params == NULL) {
                                PyText_EncodedDecref(oencoded_obj);
                                PyMem_Free(forms);
                                curl_formfree(post);
                                return NULL;
                            }
                            
                            /* Ensure that the buffer remains alive until curl_easy_cleanup() */
                            if (PyList_Append(ref_params, obj) != 0) {
                                PyText_EncodedDecref(oencoded_obj);
                                PyMem_Free(forms);
                                curl_formfree(post);
                                Py_DECREF(ref_params);
                                return NULL;
                            }

                            /* As with CURLFORM_COPYCONTENTS, specify the length. */
                            forms[k].option = CURLFORM_BUFFERLENGTH;
                            forms[k].value = (const char *)olen;
                            ++k;
                        }
                    }
                    forms[k].option = CURLFORM_END;
                    res = curl_formadd(&post, &last,
                                       CURLFORM_COPYNAME, nstr,
                                       CURLFORM_NAMELENGTH, (long) nlen,
                                       CURLFORM_ARRAY, forms,
                                       CURLFORM_END);
                    PyText_EncodedDecref(oencoded_obj);
                    PyMem_Free(forms);
                    if (res != CURLE_OK) {
                        curl_formfree(post);
                        Py_XDECREF(ref_params);
                        CURLERROR_RETVAL();
                    }
                } else {
                    /* Some other type was given, ignore */
                    PyText_EncodedDecref(nencoded_obj);
                    curl_formfree(post);
                    Py_XDECREF(ref_params);
                    PyErr_SetString(PyExc_TypeError, "unsupported second type in tuple");
                    return NULL;
                }
                PyText_EncodedDecref(nencoded_obj);
            }
            res = curl_easy_setopt(self->handle, CURLOPT_HTTPPOST, post);
            /* Check for errors */
            if (res != CURLE_OK) {
                curl_formfree(post);
                Py_XDECREF(ref_params);
                CURLERROR_RETVAL();
            }
            /* Finally, free previously allocated httppost, ZAP any
             * buffer references, and update */
            curl_formfree(self->httppost);
            util_curl_xdecref(self, PYCURL_MEMGROUP_HTTPPOST, self->handle);
            self->httppost = post;

            /* The previous list of INCed references was ZAPed above; save
             * the new one so that we can clean it up on the next
             * self->httppost free. */
            self->httppost_ref_list = ref_params;

            Py_RETURN_NONE;
        }

        /* Just to be sure we do not bug off here */
        assert(old_slist != NULL && slist == NULL);

        /* Handle regular list operations on the other options */
        for (i = 0; i < len; i++) {
            PyObject *listitem = PyList_GetItem(obj, i);
            struct curl_slist *nlist;
            char *str;
            PyObject *sencoded_obj;

            if (!PyText_Check(listitem)) {
                curl_slist_free_all(slist);
                PyErr_SetString(PyExc_TypeError, "list items must be byte strings or Unicode strings with ASCII code points only");
                return NULL;
            }
            /* INFO: curl_slist_append() internally does strdup() the data, so
             * no embedded NUL characters allowed here. */
            str = PyText_AsString_NoNUL(listitem, &sencoded_obj);
            if (str == NULL) {
                curl_slist_free_all(slist);
                return NULL;
            }
            nlist = curl_slist_append(slist, str);
            PyText_EncodedDecref(sencoded_obj);
            if (nlist == NULL || nlist->data == NULL) {
                curl_slist_free_all(slist);
                return PyErr_NoMemory();
            }
            slist = nlist;
        }
        res = curl_easy_setopt(self->handle, (CURLoption)option, slist);
        /* Check for errors */
        if (res != CURLE_OK) {
            curl_slist_free_all(slist);
            CURLERROR_RETVAL();
        }
        /* Finally, free previously allocated list and update */
        curl_slist_free_all(*old_slist);
        *old_slist = slist;

        Py_RETURN_NONE;
    }

    /* Handle the case of function objects for callbacks */
    if (PyFunction_Check(obj) || PyCFunction_Check(obj) ||
        PyCallable_Check(obj) || PyMethod_Check(obj)) {
        /* We use function types here to make sure that our callback
         * definitions exactly match the <curl/curl.h> interface.
         */
        const curl_write_callback w_cb = write_callback;
        const curl_write_callback h_cb = header_callback;
        const curl_read_callback r_cb = read_callback;
        const curl_progress_callback pro_cb = progress_callback;
        const curl_debug_callback debug_cb = debug_callback;
        const curl_ioctl_callback ioctl_cb = ioctl_callback;
        const curl_opensocket_callback opensocket_cb = opensocket_callback;
        const curl_seek_callback seek_cb = seek_callback;

        switch(option) {
        case CURLOPT_WRITEFUNCTION:
            if (self->writeheader_fp != NULL) {
                PyErr_SetString(ErrorObject, "cannot combine WRITEFUNCTION with WRITEHEADER option.");
                return NULL;
            }
            Py_INCREF(obj);
            Py_CLEAR(self->writedata_fp);
            Py_CLEAR(self->w_cb);
            self->w_cb = obj;
            curl_easy_setopt(self->handle, CURLOPT_WRITEFUNCTION, w_cb);
            curl_easy_setopt(self->handle, CURLOPT_WRITEDATA, self);
            break;
        case CURLOPT_HEADERFUNCTION:
            Py_INCREF(obj);
            Py_CLEAR(self->h_cb);
            self->h_cb = obj;
            curl_easy_setopt(self->handle, CURLOPT_HEADERFUNCTION, h_cb);
            curl_easy_setopt(self->handle, CURLOPT_WRITEHEADER, self);
            break;
        case CURLOPT_READFUNCTION:
            Py_INCREF(obj);
            Py_CLEAR(self->readdata_fp);
            Py_CLEAR(self->r_cb);
            self->r_cb = obj;
            curl_easy_setopt(self->handle, CURLOPT_READFUNCTION, r_cb);
            curl_easy_setopt(self->handle, CURLOPT_READDATA, self);
            break;
        case CURLOPT_PROGRESSFUNCTION:
            Py_INCREF(obj);
            Py_CLEAR(self->pro_cb);
            self->pro_cb = obj;
            curl_easy_setopt(self->handle, CURLOPT_PROGRESSFUNCTION, pro_cb);
            curl_easy_setopt(self->handle, CURLOPT_PROGRESSDATA, self);
            break;
        case CURLOPT_DEBUGFUNCTION:
            Py_INCREF(obj);
            Py_CLEAR(self->debug_cb);
            self->debug_cb = obj;
            curl_easy_setopt(self->handle, CURLOPT_DEBUGFUNCTION, debug_cb);
            curl_easy_setopt(self->handle, CURLOPT_DEBUGDATA, self);
            break;
        case CURLOPT_IOCTLFUNCTION:
            Py_INCREF(obj);
            Py_CLEAR(self->ioctl_cb);
            self->ioctl_cb = obj;
            curl_easy_setopt(self->handle, CURLOPT_IOCTLFUNCTION, ioctl_cb);
            curl_easy_setopt(self->handle, CURLOPT_IOCTLDATA, self);
            break;
        case CURLOPT_OPENSOCKETFUNCTION:
            Py_INCREF(obj);
            Py_CLEAR(self->opensocket_cb);
            self->opensocket_cb = obj;
            curl_easy_setopt(self->handle, CURLOPT_OPENSOCKETFUNCTION, opensocket_cb);
            curl_easy_setopt(self->handle, CURLOPT_OPENSOCKETDATA, self);
            break;
        case CURLOPT_SEEKFUNCTION:
            Py_INCREF(obj);
            Py_CLEAR(self->seek_cb);
            self->seek_cb = obj;
            curl_easy_setopt(self->handle, CURLOPT_SEEKFUNCTION, seek_cb);
            curl_easy_setopt(self->handle, CURLOPT_SEEKDATA, self);
            break;

        default:
            /* None of the function options were recognized, raise exception */
            PyErr_SetString(PyExc_TypeError, "functions are not supported for this option");
            return NULL;
        }
        Py_RETURN_NONE;
    }
    /* handle the SHARE case */
    if (option == CURLOPT_SHARE) {
        CurlShareObject *share;

        if (self->share == NULL && (obj == NULL || obj == Py_None))
            Py_RETURN_NONE;

        if (self->share) {
            if (obj != Py_None) {
                PyErr_SetString(ErrorObject, "Curl object already sharing. Unshare first.");
                return NULL;
            }
            else {
                share = self->share;
                res = curl_easy_setopt(self->handle, CURLOPT_SHARE, NULL);
                if (res != CURLE_OK) {
                    CURLERROR_RETVAL();
                }
                self->share = NULL;
                Py_DECREF(share);
                Py_RETURN_NONE;
            }
        }
        if (Py_TYPE(obj) != p_CurlShare_Type) {
            PyErr_SetString(PyExc_TypeError, "invalid arguments to setopt");
            return NULL;
        }
        share = (CurlShareObject*)obj;
        res = curl_easy_setopt(self->handle, CURLOPT_SHARE, share->share_handle);
        if (res != CURLE_OK) {
            CURLERROR_RETVAL();
        }
        self->share = share;
        Py_INCREF(share);
        Py_RETURN_NONE;
    }

    /*
    Handle the case of file-like objects for Python 3.
    
    Given an object with a write method, we will call the write method
    from the appropriate callback.
    
    Files in Python 3 are no longer FILE * instances and therefore cannot
    be directly given to curl.
    
    For consistency, ability to use any file-like object is also available
    on Python 2.
    */
    if (option == CURLOPT_READDATA ||
        option == CURLOPT_WRITEDATA ||
        option == CURLOPT_WRITEHEADER)
    {
        PyObject *write_method = PyObject_GetAttrString(obj, "write");
        if (write_method) {
            PyObject *arglist;
            PyObject *rv;
            
            switch (option) {
                case CURLOPT_READDATA:
                    option = CURLOPT_READFUNCTION;
                    break;
                case CURLOPT_WRITEDATA:
                    option = CURLOPT_WRITEFUNCTION;
                    break;
                case CURLOPT_WRITEHEADER:
                    if (self->w_cb != NULL) {
                        PyErr_SetString(ErrorObject, "cannot combine WRITEHEADER with WRITEFUNCTION.");
                        Py_DECREF(write_method);
                        return NULL;
                    }
                    option = CURLOPT_HEADERFUNCTION;
                    break;
                default:
                    PyErr_SetString(PyExc_TypeError, "objects are not supported for this option");
                    Py_DECREF(write_method);
                    return NULL;
            }
            
            arglist = Py_BuildValue("(iO)", option, write_method);
            /* reference is now in arglist */
            Py_DECREF(write_method);
            if (arglist == NULL) {
                return NULL;
            }
            rv = do_curl_setopt(self, arglist);
            Py_DECREF(arglist);
            return rv;
        } else {
            PyErr_SetString(ErrorObject, "object given without a write method");
            return NULL;
        }
    }

    /* Failed to match any of the function signatures -- return error */
error:
    PyErr_SetString(PyExc_TypeError, "invalid arguments to setopt");
    return NULL;
}


static PyObject *
do_curl_getinfo(CurlObject *self, PyObject *args)
{
    int option;
    int res;

    if (!PyArg_ParseTuple(args, "i:getinfo", &option)) {
        return NULL;
    }
    if (check_curl_state(self, 1 | 2, "getinfo") != 0) {
        return NULL;
    }

    switch (option) {
    case CURLINFO_FILETIME:
    case CURLINFO_HEADER_SIZE:
    case CURLINFO_HTTP_CODE:
    case CURLINFO_REDIRECT_COUNT:
    case CURLINFO_REQUEST_SIZE:
    case CURLINFO_SSL_VERIFYRESULT:
    case CURLINFO_HTTP_CONNECTCODE:
    case CURLINFO_HTTPAUTH_AVAIL:
    case CURLINFO_PROXYAUTH_AVAIL:
    case CURLINFO_OS_ERRNO:
    case CURLINFO_NUM_CONNECTS:
    case CURLINFO_LASTSOCKET:
#ifdef HAVE_CURLINFO_LOCAL_PORT
    case CURLINFO_LOCAL_PORT:
#endif
#ifdef HAVE_CURLINFO_PRIMARY_PORT
    case CURLINFO_PRIMARY_PORT:
#endif

        {
            /* Return PyInt as result */
            long l_res = -1;

            res = curl_easy_getinfo(self->handle, (CURLINFO)option, &l_res);
            /* Check for errors and return result */
            if (res != CURLE_OK) {
                CURLERROR_RETVAL();
            }
            return PyInt_FromLong(l_res);
        }

    case CURLINFO_CONTENT_TYPE:
    case CURLINFO_EFFECTIVE_URL:
    case CURLINFO_FTP_ENTRY_PATH:
    case CURLINFO_REDIRECT_URL:
    case CURLINFO_PRIMARY_IP:
#ifdef HAVE_CURLINFO_LOCAL_IP
    case CURLINFO_LOCAL_IP:
#endif
        {
            /* Return PyString as result */
            char *s_res = NULL;

            res = curl_easy_getinfo(self->handle, (CURLINFO)option, &s_res);
            if (res != CURLE_OK) {
                CURLERROR_RETVAL();
            }
            /* If the resulting string is NULL, return None */
            if (s_res == NULL) {
                Py_RETURN_NONE;
            }
            return PyText_FromString(s_res);

        }

    case CURLINFO_CONNECT_TIME:
    case CURLINFO_APPCONNECT_TIME:
    case CURLINFO_CONTENT_LENGTH_DOWNLOAD:
    case CURLINFO_CONTENT_LENGTH_UPLOAD:
    case CURLINFO_NAMELOOKUP_TIME:
    case CURLINFO_PRETRANSFER_TIME:
    case CURLINFO_REDIRECT_TIME:
    case CURLINFO_SIZE_DOWNLOAD:
    case CURLINFO_SIZE_UPLOAD:
    case CURLINFO_SPEED_DOWNLOAD:
    case CURLINFO_SPEED_UPLOAD:
    case CURLINFO_STARTTRANSFER_TIME:
    case CURLINFO_TOTAL_TIME:
        {
            /* Return PyFloat as result */
            double d_res = 0.0;

            res = curl_easy_getinfo(self->handle, (CURLINFO)option, &d_res);
            if (res != CURLE_OK) {
                CURLERROR_RETVAL();
            }
            return PyFloat_FromDouble(d_res);
        }

    case CURLINFO_SSL_ENGINES:
    case CURLINFO_COOKIELIST:
        {
            /* Return a list of strings */
            struct curl_slist *slist = NULL;

            res = curl_easy_getinfo(self->handle, (CURLINFO)option, &slist);
            if (res != CURLE_OK) {
                CURLERROR_RETVAL();
            }
            return convert_slist(slist, 1 | 2);
        }

#ifdef HAVE_CURLOPT_CERTINFO
    case CURLINFO_CERTINFO:
        {
            /* Return a list of lists of 2-tuples */
            struct curl_certinfo *clist = NULL;
            res = curl_easy_getinfo(self->handle, CURLINFO_CERTINFO, &clist);
            if (res != CURLE_OK) {
                CURLERROR_RETVAL();
            } else {
                return convert_certinfo(clist);
            }
        }
#endif
    }

    /* Got wrong option on the method call */
    PyErr_SetString(PyExc_ValueError, "invalid argument to getinfo");
    return NULL;
}

/* curl_easy_pause() can be called from inside a callback or outside */
static PyObject *
do_curl_pause(CurlObject *self, PyObject *args)
{
    int bitmask;
    CURLcode res;
#ifdef WITH_THREAD
    PyThreadState *saved_state;
#endif

    if (!PyArg_ParseTuple(args, "i:pause", &bitmask)) {
        return NULL;
    }
    if (check_curl_state(self, 1, "pause") != 0) {
        return NULL;
    }

#ifdef WITH_THREAD
    /* Save handle to current thread (used as context for python callbacks) */
    saved_state = self->state;
    PYCURL_BEGIN_ALLOW_THREADS

    /* We must allow threads here because unpausing a handle can cause
       some of its callbacks to be invoked immediately, from inside
       curl_easy_pause() */
#endif

    res = curl_easy_pause(self->handle, bitmask);

#ifdef WITH_THREAD
    PYCURL_END_ALLOW_THREADS

    /* Restore the thread-state to whatever it was on entry */
    self->state = saved_state;
#endif

    if (res != CURLE_OK) {
        CURLERROR_MSG("pause/unpause failed");
    } else {
        Py_INCREF(Py_None);
        return Py_None;
    }
}

static const char co_pause_doc [] =
    "pause(bitmask) -> None.  "
    "Pauses or unpauses a curl handle. Bitmask should be a value such as PAUSE_RECV or PAUSE_CONT.  "
    "Raises pycurl.error exception upon failure.\n";

/*************************************************************************
// CurlMultiObject
**************************************************************************/

/* --------------- construct/destruct (i.e. open/close) --------------- */

/* constructor - this is a module-level function returning a new instance */
PYCURL_INTERNAL CurlMultiObject *
do_multi_new(PyObject *dummy)
{
    CurlMultiObject *self;

    UNUSED(dummy);

    /* Allocate python curl-multi object */
    self = (CurlMultiObject *) PyObject_GC_New(CurlMultiObject, p_CurlMulti_Type);
    if (self) {
        PyObject_GC_Track(self);
    }
    else {
        return NULL;
    }

    /* Initialize object attributes */
    self->dict = NULL;
#ifdef WITH_THREAD
    self->state = NULL;
#endif
    self->t_cb = NULL;
    self->s_cb = NULL;

    /* Allocate libcurl multi handle */
    self->multi_handle = curl_multi_init();
    if (self->multi_handle == NULL) {
        Py_DECREF(self);
        PyErr_SetString(ErrorObject, "initializing curl-multi failed");
        return NULL;
    }
    return self;
}

static void
util_multi_close(CurlMultiObject *self)
{
    assert(self != NULL);
#ifdef WITH_THREAD
    self->state = NULL;
#endif
    if (self->multi_handle != NULL) {
        CURLM *multi_handle = self->multi_handle;
        self->multi_handle = NULL;
        curl_multi_cleanup(multi_handle);
    }
}


PYCURL_INTERNAL void
do_multi_dealloc(CurlMultiObject *self)
{
    PyObject_GC_UnTrack(self);
    Py_TRASHCAN_SAFE_BEGIN(self)

    Py_CLEAR(self->dict);
    util_multi_close(self);

    PyObject_GC_Del(self);
    Py_TRASHCAN_SAFE_END(self)
}


static PyObject *
do_multi_close(CurlMultiObject *self)
{
    if (check_multi_state(self, 2, "close") != 0) {
        return NULL;
    }
    util_multi_close(self);
    Py_RETURN_NONE;
}


/* --------------- GC support --------------- */

/* Drop references that may have created reference cycles. */
PYCURL_INTERNAL int
do_multi_clear(CurlMultiObject *self)
{
    Py_CLEAR(self->dict);
    return 0;
}

PYCURL_INTERNAL int
do_multi_traverse(CurlMultiObject *self, visitproc visit, void *arg)
{
    int err;
#undef VISIT
#define VISIT(v)    if ((v) != NULL && ((err = visit(v, arg)) != 0)) return err

    VISIT(self->dict);

    return 0;
#undef VISIT
}


/* --------------- setopt --------------- */

static int
multi_socket_callback(CURL *easy,
                      curl_socket_t s,
                      int what,
                      void *userp,
                      void *socketp)
{
    CurlMultiObject *self;
    PyObject *arglist;
    PyObject *result = NULL;
    PYCURL_DECLARE_THREAD_STATE;

    /* acquire thread */
    self = (CurlMultiObject *)userp;
    if (!PYCURL_ACQUIRE_THREAD_MULTI())
        return 0;

    /* check args */
    if (self->s_cb == NULL)
        goto silent_error;

    if (socketp == NULL) {
        Py_INCREF(Py_None);
        socketp = Py_None;
    }

    /* run callback */
    arglist = Py_BuildValue("(iiOO)", what, s, userp, (PyObject *)socketp);
    if (arglist == NULL)
        goto verbose_error;
    result = PyEval_CallObject(self->s_cb, arglist);
    Py_DECREF(arglist);
    if (result == NULL)
        goto verbose_error;

    /* return values from socket callbacks should be ignored */

silent_error:
    Py_XDECREF(result);
    PYCURL_RELEASE_THREAD();
    return 0;
verbose_error:
    PyErr_Print();
    goto silent_error;
    return 0;
}


static int
multi_timer_callback(CURLM *multi,
                     long timeout_ms,
                     void *userp)
{
    CurlMultiObject *self;
    PyObject *arglist;
    PyObject *result = NULL;
    int ret = 0;       /* always success */
    PYCURL_DECLARE_THREAD_STATE;

    UNUSED(multi);

    /* acquire thread */
    self = (CurlMultiObject *)userp;
    if (!PYCURL_ACQUIRE_THREAD_MULTI())
        return ret;

    /* check args */
    if (self->t_cb == NULL)
        goto silent_error;

    /* run callback */
    arglist = Py_BuildValue("(i)", timeout_ms);
    if (arglist == NULL)
        goto verbose_error;
    result = PyEval_CallObject(self->t_cb, arglist);
    Py_DECREF(arglist);
    if (result == NULL)
        goto verbose_error;

    /* return values from timer callbacks should be ignored */

silent_error:
    Py_XDECREF(result);
    PYCURL_RELEASE_THREAD();
    return ret;
verbose_error:
    PyErr_Print();
    goto silent_error;

    return 0;
}


static PyObject *
do_multi_setopt(CurlMultiObject *self, PyObject *args)
{
    int option;
    PyObject *obj;

    if (!PyArg_ParseTuple(args, "iO:setopt", &option, &obj))
        return NULL;
    if (check_multi_state(self, 1 | 2, "setopt") != 0)
        return NULL;

    /* Early checks of option value */
    if (option <= 0)
        goto error;
    if (option >= (int)CURLOPTTYPE_OFF_T + MOPTIONS_SIZE)
        goto error;
    if (option % 10000 >= MOPTIONS_SIZE)
        goto error;

    /* Handle the case of integer arguments */
    if (PyInt_Check(obj)) {
        long d = PyInt_AsLong(obj);
        switch(option) {
        case CURLMOPT_MAXCONNECTS:
        case CURLMOPT_PIPELINING:
#ifdef HAVE_CURL_7_30_0_PIPELINE_OPTS
        case CURLMOPT_MAX_HOST_CONNECTIONS:
        case CURLMOPT_MAX_TOTAL_CONNECTIONS:
        case CURLMOPT_MAX_PIPELINE_LENGTH:
	case CURLMOPT_CONTENT_LENGTH_PENALTY_SIZE:
	case CURLMOPT_CHUNK_LENGTH_PENALTY_SIZE:
#endif
            curl_multi_setopt(self->multi_handle, option, d);
            break;
        default:
            PyErr_SetString(PyExc_TypeError, "integers are not supported for this option");
            return NULL;
        }
        Py_RETURN_NONE;
    }
    if (PyFunction_Check(obj) || PyCFunction_Check(obj) ||
        PyCallable_Check(obj) || PyMethod_Check(obj)) {
        /* We use function types here to make sure that our callback
         * definitions exactly match the <curl/multi.h> interface.
         */
        const curl_multi_timer_callback t_cb = multi_timer_callback;
        const curl_socket_callback s_cb = multi_socket_callback;

        switch(option) {
        case CURLMOPT_SOCKETFUNCTION:
            curl_multi_setopt(self->multi_handle, CURLMOPT_SOCKETFUNCTION, s_cb);
            curl_multi_setopt(self->multi_handle, CURLMOPT_SOCKETDATA, self);
            Py_INCREF(obj);
            self->s_cb = obj;
            break;
        case CURLMOPT_TIMERFUNCTION:
            curl_multi_setopt(self->multi_handle, CURLMOPT_TIMERFUNCTION, t_cb);
            curl_multi_setopt(self->multi_handle, CURLMOPT_TIMERDATA, self);
            Py_INCREF(obj);
            self->t_cb = obj;
            break;
        default:
            PyErr_SetString(PyExc_TypeError, "callables are not supported for this option");
            return NULL;
        }
        Py_RETURN_NONE;
    }
    /* Failed to match any of the function signatures -- return error */
error:
    PyErr_SetString(PyExc_TypeError, "invalid arguments to setopt");
    return NULL;
}


/* --------------- timeout --------------- */

static PyObject *
do_multi_timeout(CurlMultiObject *self)
{
    CURLMcode res;
    long timeout;

    if (check_multi_state(self, 1 | 2, "timeout") != 0) {
        return NULL;
    }

    res = curl_multi_timeout(self->multi_handle, &timeout);
    if (res != CURLM_OK) {
        CURLERROR_MSG("timeout failed");
    }

    /* Return number of millisecs until timeout */
    return Py_BuildValue("l", timeout);
}


/* --------------- assign --------------- */

static PyObject *
do_multi_assign(CurlMultiObject *self, PyObject *args)
{
    CURLMcode res;
    curl_socket_t socket;
    PyObject *obj;

    if (!PyArg_ParseTuple(args, "iO:assign", &socket, &obj))
        return NULL;
    if (check_multi_state(self, 1 | 2, "assign") != 0) {
        return NULL;
    }
    Py_INCREF(obj);

    res = curl_multi_assign(self->multi_handle, socket, obj);
    if (res != CURLM_OK) {
        CURLERROR_MSG("assign failed");
    }

    Py_RETURN_NONE;
}


/* --------------- socket_action --------------- */
static PyObject *
do_multi_socket_action(CurlMultiObject *self, PyObject *args)
{
    CURLMcode res;
    curl_socket_t socket;
    int ev_bitmask;
    int running = -1;
    
    if (!PyArg_ParseTuple(args, "ii:socket_action", &socket, &ev_bitmask))
        return NULL;
    if (check_multi_state(self, 1 | 2, "socket_action") != 0) {
        return NULL;
    }

    PYCURL_BEGIN_ALLOW_THREADS
    res = curl_multi_socket_action(self->multi_handle, socket, ev_bitmask, &running);
    PYCURL_END_ALLOW_THREADS

    if (res != CURLM_OK) {
        CURLERROR_MSG("multi_socket_action failed");
    }
    /* Return a tuple with the result and the number of running handles */
    return Py_BuildValue("(ii)", (int)res, running);
}

/* --------------- socket_all --------------- */

static PyObject *
do_multi_socket_all(CurlMultiObject *self)
{
    CURLMcode res;
    int running = -1;

    if (check_multi_state(self, 1 | 2, "socket_all") != 0) {
        return NULL;
    }

    PYCURL_BEGIN_ALLOW_THREADS
    res = curl_multi_socket_all(self->multi_handle, &running);
    PYCURL_END_ALLOW_THREADS

    /* We assume these errors are ok, otherwise raise exception */
    if (res != CURLM_OK && res != CURLM_CALL_MULTI_PERFORM) {
        CURLERROR_MSG("perform failed");
    }

    /* Return a tuple with the result and the number of running handles */
    return Py_BuildValue("(ii)", (int)res, running);
}


/* --------------- perform --------------- */

static PyObject *
do_multi_perform(CurlMultiObject *self)
{
    CURLMcode res;
    int running = -1;

    if (check_multi_state(self, 1 | 2, "perform") != 0) {
        return NULL;
    }

    PYCURL_BEGIN_ALLOW_THREADS
    res = curl_multi_perform(self->multi_handle, &running);
    PYCURL_END_ALLOW_THREADS

    /* We assume these errors are ok, otherwise raise exception */
    if (res != CURLM_OK && res != CURLM_CALL_MULTI_PERFORM) {
        CURLERROR_MSG("perform failed");
    }

    /* Return a tuple with the result and the number of running handles */
    return Py_BuildValue("(ii)", (int)res, running);
}


/* --------------- add_handle/remove_handle --------------- */

/* static utility function */
static int
check_multi_add_remove(const CurlMultiObject *self, const CurlObject *obj)
{
    /* check CurlMultiObject status */
    assert_multi_state(self);
    if (self->multi_handle == NULL) {
        PyErr_SetString(ErrorObject, "cannot add/remove handle - multi-stack is closed");
        return -1;
    }
#ifdef WITH_THREAD
    if (self->state != NULL) {
        PyErr_SetString(ErrorObject, "cannot add/remove handle - multi_perform() already running");
        return -1;
    }
#endif
    /* check CurlObject status */
    assert_curl_state(obj);
#ifdef WITH_THREAD
    if (obj->state != NULL) {
        PyErr_SetString(ErrorObject, "cannot add/remove handle - perform() of curl object already running");
        return -1;
    }
#endif
    if (obj->multi_stack != NULL && obj->multi_stack != self) {
        PyErr_SetString(ErrorObject, "cannot add/remove handle - curl object already on another multi-stack");
        return -1;
    }
    return 0;
}


static PyObject *
do_multi_add_handle(CurlMultiObject *self, PyObject *args)
{
    CurlObject *obj;
    CURLMcode res;

    if (!PyArg_ParseTuple(args, "O!:add_handle", p_Curl_Type, &obj)) {
        return NULL;
    }
    if (check_multi_add_remove(self, obj) != 0) {
        return NULL;
    }
    if (obj->handle == NULL) {
        PyErr_SetString(ErrorObject, "curl object already closed");
        return NULL;
    }
    if (obj->multi_stack == self) {
        PyErr_SetString(ErrorObject, "curl object already on this multi-stack");
        return NULL;
    }
    assert(obj->multi_stack == NULL);
    res = curl_multi_add_handle(self->multi_handle, obj->handle);
    if (res != CURLM_OK) {
        CURLERROR_MSG("curl_multi_add_handle() failed due to internal errors");
    }
    obj->multi_stack = self;
    Py_INCREF(self);
    Py_RETURN_NONE;
}


static PyObject *
do_multi_remove_handle(CurlMultiObject *self, PyObject *args)
{
    CurlObject *obj;
    CURLMcode res;

    if (!PyArg_ParseTuple(args, "O!:remove_handle", p_Curl_Type, &obj)) {
        return NULL;
    }
    if (check_multi_add_remove(self, obj) != 0) {
        return NULL;
    }
    if (obj->handle == NULL) {
        /* CurlObject handle already closed -- ignore */
        goto done;
    }
    if (obj->multi_stack != self) {
        PyErr_SetString(ErrorObject, "curl object not on this multi-stack");
        return NULL;
    }
    res = curl_multi_remove_handle(self->multi_handle, obj->handle);
    if (res != CURLM_OK) {
        CURLERROR_MSG("curl_multi_remove_handle() failed due to internal errors");
    }
    assert(obj->multi_stack == self);
    obj->multi_stack = NULL;
    Py_DECREF(self);
done:
    Py_RETURN_NONE;
}


/* --------------- fdset ---------------------- */

static PyObject *
do_multi_fdset(CurlMultiObject *self)
{
    CURLMcode res;
    int max_fd = -1, fd;
    PyObject *ret = NULL;
    PyObject *read_list = NULL, *write_list = NULL, *except_list = NULL;
    PyObject *py_fd = NULL;

    if (check_multi_state(self, 1 | 2, "fdset") != 0) {
        return NULL;
    }

    /* Clear file descriptor sets */
    FD_ZERO(&self->read_fd_set);
    FD_ZERO(&self->write_fd_set);
    FD_ZERO(&self->exc_fd_set);

    /* Don't bother releasing the gil as this is just a data structure operation */
    res = curl_multi_fdset(self->multi_handle, &self->read_fd_set,
                           &self->write_fd_set, &self->exc_fd_set, &max_fd);
    if (res != CURLM_OK) {
        CURLERROR_MSG("curl_multi_fdset() failed due to internal errors");
    }

    /* Allocate lists. */
    if ((read_list = PyList_New((Py_ssize_t)0)) == NULL) goto error;
    if ((write_list = PyList_New((Py_ssize_t)0)) == NULL) goto error;
    if ((except_list = PyList_New((Py_ssize_t)0)) == NULL) goto error;

    /* Populate lists */
    for (fd = 0; fd < max_fd + 1; fd++) {
        if (FD_ISSET(fd, &self->read_fd_set)) {
            if ((py_fd = PyInt_FromLong((long)fd)) == NULL) goto error;
            if (PyList_Append(read_list, py_fd) != 0) goto error;
            Py_DECREF(py_fd);
            py_fd = NULL;
        }
        if (FD_ISSET(fd, &self->write_fd_set)) {
            if ((py_fd = PyInt_FromLong((long)fd)) == NULL) goto error;
            if (PyList_Append(write_list, py_fd) != 0) goto error;
            Py_DECREF(py_fd);
            py_fd = NULL;
        }
        if (FD_ISSET(fd, &self->exc_fd_set)) {
            if ((py_fd = PyInt_FromLong((long)fd)) == NULL) goto error;
            if (PyList_Append(except_list, py_fd) != 0) goto error;
            Py_DECREF(py_fd);
            py_fd = NULL;
        }
    }

    /* Return a tuple with the 3 lists */
    ret = Py_BuildValue("(OOO)", read_list, write_list, except_list);
error:
    Py_XDECREF(py_fd);
    Py_XDECREF(except_list);
    Py_XDECREF(write_list);
    Py_XDECREF(read_list);
    return ret;
}


/* --------------- info_read --------------- */

static PyObject *
do_multi_info_read(CurlMultiObject *self, PyObject *args)
{
    PyObject *ret = NULL;
    PyObject *ok_list = NULL, *err_list = NULL;
    CURLMsg *msg;
    int in_queue = 0, num_results = INT_MAX;

    /* Sanity checks */
    if (!PyArg_ParseTuple(args, "|i:info_read", &num_results)) {
        return NULL;
    }
    if (num_results <= 0) {
        PyErr_SetString(ErrorObject, "argument to info_read must be greater than zero");
        return NULL;
    }
    if (check_multi_state(self, 1 | 2, "info_read") != 0) {
        return NULL;
    }

    if ((ok_list = PyList_New((Py_ssize_t)0)) == NULL) goto error;
    if ((err_list = PyList_New((Py_ssize_t)0)) == NULL) goto error;

    /* Loop through all messages */
    while ((msg = curl_multi_info_read(self->multi_handle, &in_queue)) != NULL) {
        CURLcode res;
        CurlObject *co = NULL;

        /* Check for termination as specified by the user */
        if (num_results-- <= 0) {
            break;
        }

        /* Fetch the curl object that corresponds to the curl handle in the message */
        res = curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &co);
        if (res != CURLE_OK || co == NULL) {
            Py_DECREF(err_list);
            Py_DECREF(ok_list);
            CURLERROR_MSG("Unable to fetch curl handle from curl object");
        }
        assert(Py_TYPE(co) == p_Curl_Type);
        if (msg->msg != CURLMSG_DONE) {
            /* FIXME: what does this mean ??? */
        }
        if (msg->data.result == CURLE_OK) {
            /* Append curl object to list of objects which succeeded */
            if (PyList_Append(ok_list, (PyObject *)co) != 0) {
                goto error;
            }
        }
        else {
            /* Create a result tuple that will get added to err_list. */
            PyObject *v = Py_BuildValue("(Ois)", (PyObject *)co, (int)msg->data.result, co->error);
            /* Append curl object to list of objects which failed */
            if (v == NULL || PyList_Append(err_list, v) != 0) {
                Py_XDECREF(v);
                goto error;
            }
            Py_DECREF(v);
        }
    }
    /* Return (number of queued messages, [ok_objects], [error_objects]) */
    ret = Py_BuildValue("(iOO)", in_queue, ok_list, err_list);
error:
    Py_XDECREF(err_list);
    Py_XDECREF(ok_list);
    return ret;
}


/* --------------- select --------------- */

static PyObject *
do_multi_select(CurlMultiObject *self, PyObject *args)
{
    int max_fd = -1, n;
    double timeout = -1.0;
    struct timeval tv, *tvp;
    CURLMcode res;

    if (!PyArg_ParseTuple(args, "d:select", &timeout)) {
        return NULL;
    }
    if (check_multi_state(self, 1 | 2, "select") != 0) {
        return NULL;
    }

    if (timeout < 0 || timeout >= 365 * 24 * 60 * 60) {
        PyErr_SetString(PyExc_OverflowError, "invalid timeout period");
        return NULL;
    } else {
        long seconds = (long)timeout;
        timeout = timeout - (double)seconds;
        assert(timeout >= 0.0); assert(timeout < 1.0);
        tv.tv_sec = seconds;
        tv.tv_usec = (long)(timeout*1000000.0);
        tvp = &tv;
    }

    FD_ZERO(&self->read_fd_set);
    FD_ZERO(&self->write_fd_set);
    FD_ZERO(&self->exc_fd_set);

    res = curl_multi_fdset(self->multi_handle, &self->read_fd_set,
                           &self->write_fd_set, &self->exc_fd_set, &max_fd);
    if (res != CURLM_OK) {
        CURLERROR_MSG("multi_fdset failed");
    }

    if (max_fd < 0) {
        n = 0;
    }
    else {
        Py_BEGIN_ALLOW_THREADS
        n = select(max_fd + 1, &self->read_fd_set, &self->write_fd_set, &self->exc_fd_set, tvp);
        Py_END_ALLOW_THREADS
        /* info: like Python's socketmodule.c we do not raise an exception
         *       if select() fails - we'll leave it to the actual libcurl
         *       socket code to report any errors.
         */
    }

    return PyInt_FromLong(n);
}


/*************************************************************************
// type definitions
**************************************************************************/

/* --------------- methods --------------- */

static const char cso_close_doc [] =
    "close() -> None.  "
    "Close shared handle.\n";
static const char cso_setopt_doc [] =
    "setopt(option, parameter) -> None.  "
    "Set curl share option.  Raises pycurl.error exception upon failure.\n";

static const char co_close_doc [] =
    "close() -> None.  "
    "Close handle and end curl session.\n";
static const char co_errstr_doc [] =
    "errstr() -> String.  "
    "Return the internal libcurl error buffer string.\n";
static const char co_getinfo_doc [] =
    "getinfo(info) -> Res.  "
    "Extract and return information from a curl session.  Raises pycurl.error exception upon failure.\n";
static const char co_perform_doc [] =
    "perform() -> None.  "
    "Perform a file transfer.  Raises pycurl.error exception upon failure.\n";
static const char co_setopt_doc [] =
    "setopt(option, parameter) -> None.  "
    "Set curl session option.  Raises pycurl.error exception upon failure.\n";
static const char co_unsetopt_doc [] =
    "unsetopt(option) -> None.  "
    "Reset curl session option to default value.  Raises pycurl.error exception upon failure.\n";
static const char co_reset_doc [] =
    "reset() -> None. "
    "Reset all options set on curl handle to default values, but preserves live connections, session ID cache, DNS cache, cookies, and shares.\n";

static const char co_multi_close_doc [] = "\
close() -> None\n\
\n\
Corresponds to `curl_multi_cleanup`_ in libcurl. This method is\n\
automatically called by pycurl when a CurlMulti object no longer has any\n\
references to it, but can also be called explicitly.\
";
static const char co_multi_fdset_doc [] =
    "fdset() -> Tuple.  "
    "Returns a tuple of three lists that can be passed to the select.select() method .\n";
static const char co_multi_info_read_doc [] =
    "info_read([max_objects]) -> Tuple. "
    "Returns a tuple (number of queued handles, [curl objects]).\n";
static const char co_multi_select_doc [] =
    "select([timeout]) -> Int.  "
    "Returns result from doing a select() on the curl multi file descriptor with the given timeout.\n";
static const char co_multi_socket_action_doc [] =
    "socket_action(sockfd, ev_bitmask) -> Tuple.  "
    "Returns result from doing a socket_action() on the curl multi file descriptor with the given timeout.\n";
static const char co_multi_socket_all_doc [] =
    "socket_all() -> Tuple.  "
    "Returns result from doing a socket_all() on the curl multi file descriptor with the given timeout.\n";

PYCURL_INTERNAL PyMethodDef curlshareobject_methods[] = {
    {"close", (PyCFunction)do_share_close, METH_NOARGS, cso_close_doc},
    {"setopt", (PyCFunction)do_curlshare_setopt, METH_VARARGS, cso_setopt_doc},
    {NULL, NULL, 0, 0}
};

PYCURL_INTERNAL PyMethodDef curlobject_methods[] = {
    {"close", (PyCFunction)do_curl_close, METH_NOARGS, co_close_doc},
    {"errstr", (PyCFunction)do_curl_errstr, METH_NOARGS, co_errstr_doc},
    {"getinfo", (PyCFunction)do_curl_getinfo, METH_VARARGS, co_getinfo_doc},
    {"pause", (PyCFunction)do_curl_pause, METH_VARARGS, co_pause_doc},
    {"perform", (PyCFunction)do_curl_perform, METH_NOARGS, co_perform_doc},
    {"setopt", (PyCFunction)do_curl_setopt, METH_VARARGS, co_setopt_doc},
    {"unsetopt", (PyCFunction)do_curl_unsetopt, METH_VARARGS, co_unsetopt_doc},
    {"reset", (PyCFunction)do_curl_reset, METH_NOARGS, co_reset_doc},
    {NULL, NULL, 0, NULL}
};

PYCURL_INTERNAL PyMethodDef curlmultiobject_methods[] = {
    {"add_handle", (PyCFunction)do_multi_add_handle, METH_VARARGS, NULL},
    {"close", (PyCFunction)do_multi_close, METH_NOARGS, co_multi_close_doc},
    {"fdset", (PyCFunction)do_multi_fdset, METH_NOARGS, co_multi_fdset_doc},
    {"info_read", (PyCFunction)do_multi_info_read, METH_VARARGS, co_multi_info_read_doc},
    {"perform", (PyCFunction)do_multi_perform, METH_NOARGS, NULL},
    {"socket_action", (PyCFunction)do_multi_socket_action, METH_VARARGS, co_multi_socket_action_doc},
    {"socket_all", (PyCFunction)do_multi_socket_all, METH_NOARGS, co_multi_socket_all_doc},
    {"setopt", (PyCFunction)do_multi_setopt, METH_VARARGS, NULL},
    {"timeout", (PyCFunction)do_multi_timeout, METH_NOARGS, NULL},
    {"assign", (PyCFunction)do_multi_assign, METH_VARARGS, NULL},
    {"remove_handle", (PyCFunction)do_multi_remove_handle, METH_VARARGS, NULL},
    {"select", (PyCFunction)do_multi_select, METH_VARARGS, co_multi_select_doc},
    {NULL, NULL, 0, NULL}
};


/* --------------- setattr/getattr --------------- */


#if PY_MAJOR_VERSION >= 3
static PyObject *
my_getattro(PyObject *co, PyObject *name, PyObject *dict1, PyObject *dict2, PyMethodDef *m)
{
    PyObject *v = NULL;
    if( dict1 != NULL )
        v = PyDict_GetItem(dict1, name);
    if( v == NULL && dict2 != NULL )
        v = PyDict_GetItem(dict2, name);
    if( v != NULL )
    {
        Py_INCREF(v);
        return v;
    }
    PyErr_SetString(PyExc_AttributeError, "trying to obtain a non-existing attribute");
    return NULL;
}

static int
my_setattro(PyObject **dict, PyObject *name, PyObject *v)
{
    if( *dict == NULL )
    {
        *dict = PyDict_New();
        if( *dict == NULL )
            return -1;
    }
    if (v != NULL)
        return PyDict_SetItem(*dict, name, v);
    else {
        int v = PyDict_DelItem(*dict, name);
        if (v != 0) {
            /* need to convert KeyError to AttributeError */
            if (PyErr_ExceptionMatches(PyExc_KeyError)) {
                PyErr_SetString(PyExc_AttributeError, "trying to delete a non-existing attribute");
            }
        }
        return v;
    }
}

PYCURL_INTERNAL PyObject *
do_curl_getattro(PyObject *o, PyObject *n)
{
    PyObject *v = PyObject_GenericGetAttr(o, n);
    if( !v && PyErr_ExceptionMatches(PyExc_AttributeError) )
    {
        PyErr_Clear();
        v = my_getattro(o, n, ((CurlObject *)o)->dict,
                        curlobject_constants, curlobject_methods);
    }
    return v;
}

PYCURL_INTERNAL int
do_curl_setattro(PyObject *o, PyObject *name, PyObject *v)
{
    assert_curl_state((CurlObject *)o);
    return my_setattro(&((CurlObject *)o)->dict, name, v);
}

PYCURL_INTERNAL PyObject *
do_multi_getattro(PyObject *o, PyObject *n)
{
    PyObject *v;
    assert_multi_state((CurlMultiObject *)o);
    v = PyObject_GenericGetAttr(o, n);
    if( !v && PyErr_ExceptionMatches(PyExc_AttributeError) )
    {
        PyErr_Clear();
        v = my_getattro(o, n, ((CurlMultiObject *)o)->dict,
                        curlmultiobject_constants, curlmultiobject_methods);
    }
    return v;
}

PYCURL_INTERNAL int
do_multi_setattro(PyObject *o, PyObject *n, PyObject *v)
{
    assert_multi_state((CurlMultiObject *)o);
    return my_setattro(&((CurlMultiObject *)o)->dict, n, v);
}

PYCURL_INTERNAL PyObject *
do_share_getattro(PyObject *o, PyObject *n)
{
    PyObject *v;
    assert_share_state((CurlShareObject *)o);
    v = PyObject_GenericGetAttr(o, n);
    if( !v && PyErr_ExceptionMatches(PyExc_AttributeError) )
    {
        PyErr_Clear();
        v = my_getattro(o, n, ((CurlShareObject *)o)->dict,
                        curlshareobject_constants, curlshareobject_methods);
    }
    return v;
}

PYCURL_INTERNAL int
do_share_setattro(PyObject *o, PyObject *n, PyObject *v)
{
    assert_share_state((CurlShareObject *)o);
    return my_setattro(&((CurlShareObject *)o)->dict, n, v);
}

#else
static int
my_setattr(PyObject **dict, char *name, PyObject *v)
{
    if (v == NULL) {
        int rv = -1;
        if (*dict != NULL)
            rv = PyDict_DelItemString(*dict, name);
        if (rv < 0)
            PyErr_SetString(PyExc_AttributeError, "delete non-existing attribute");
        return rv;
    }
    if (*dict == NULL) {
        *dict = PyDict_New();
        if (*dict == NULL)
            return -1;
    }
    return PyDict_SetItemString(*dict, name, v);
}

static PyObject *
my_getattr(PyObject *co, char *name, PyObject *dict1, PyObject *dict2, PyMethodDef *m)
{
    PyObject *v = NULL;
    if (v == NULL && dict1 != NULL)
        v = PyDict_GetItemString(dict1, name);
    if (v == NULL && dict2 != NULL)
        v = PyDict_GetItemString(dict2, name);
    if (v != NULL) {
        Py_INCREF(v);
        return v;
    }
    return Py_FindMethod(m, co, name);
}

PYCURL_INTERNAL int
do_share_setattr(CurlShareObject *so, char *name, PyObject *v)
{
    assert_share_state(so);
    return my_setattr(&so->dict, name, v);
}

PYCURL_INTERNAL int
do_curl_setattr(CurlObject *co, char *name, PyObject *v)
{
    assert_curl_state(co);
    return my_setattr(&co->dict, name, v);
}

PYCURL_INTERNAL int
do_multi_setattr(CurlMultiObject *co, char *name, PyObject *v)
{
    assert_multi_state(co);
    return my_setattr(&co->dict, name, v);
}

PYCURL_INTERNAL PyObject *
do_share_getattr(CurlShareObject *cso, char *name)
{
    assert_share_state(cso);
    return my_getattr((PyObject *)cso, name, cso->dict,
                      curlshareobject_constants, curlshareobject_methods);
}

PYCURL_INTERNAL PyObject *
do_curl_getattr(CurlObject *co, char *name)
{
    assert_curl_state(co);
    return my_getattr((PyObject *)co, name, co->dict,
                      curlobject_constants, curlobject_methods);
}

PYCURL_INTERNAL PyObject *
do_multi_getattr(CurlMultiObject *co, char *name)
{
    assert_multi_state(co);
    return my_getattr((PyObject *)co, name, co->dict,
                      curlmultiobject_constants, curlmultiobject_methods);
}
#endif

/* vi:ts=4:et:nowrap
 */
