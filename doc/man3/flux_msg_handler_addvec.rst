==========================
flux_msg_handler_addvec(3)
==========================


SYNOPSIS
========

::

   #include <flux/core.h>

::

   struct flux_msg_handler_spec {
       int typemask;
       const char *topic_glob;
       flux_msg_handler_f cb;
       uint32_t rolemask;
   };

::

   int flux_msg_handler_addvec (flux_t *h,
                                const struct flux_msg_handler_spec tab[],
                                void *arg,
                                flux_msg_handler_t **handlers[]);

::

   void flux_msg_handler_delvec (flux_msg_handler_t *handlers[]);


DESCRIPTION
===========

``flux_msg_handler_addvec()`` creates and starts an array of message handlers,
terminated by FLUX_MSGHANDLER_TABLE_END. The new message handler objects
are assigned to an internally allocated array, returned in *handlers*.
The last entry in the array is set to NULL.

``flux_msg_handler_delvec()`` stops and destroys an array of message handlers
returned from ``flux_msg_handler_addvec()``.

These functions are convenience functions which call
``flux_msg_handler_create(3)``, ``flux_msg_handler_start(3)``; and
``flux_msg_handler_stop(3)``, ``flux_msg_handler_destroy(3)`` on each element
of the array, respectively.

If ``flux_msg_handler_addvec()`` encounters an error creating a message
handler, all previously created message handlers in the array are destroyed
before an error is returned.


RETURN VALUE
============

``flux_msg_handler_addvec()`` returns zero on success.
On error, -1 is returned, and errno is set appropriately.


ERRORS
======

ENOMEM
   Out of memory.


RESOURCES
=========

Github: http://github.com/flux-framework


SEE ALSO
========

flux_msg_handler_create(3)
