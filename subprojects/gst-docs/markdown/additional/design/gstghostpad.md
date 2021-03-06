# Ghostpads

GhostPads are used to build complex compound elements out of existing
elements. They are used to expose internal element pads on the complex
element.

## Some design requirements

- Must look like a real `GstPad` on both sides.
- target of Ghostpad must be changeable
- target can be initially NULL

- a GhostPad is implemented using a private `GstProxyPad` class:

```
GstProxyPad
(------------------)
| GstPad           |
|------------------|
| GstPad *target   |
(------------------)
| GstPad *internal |
(------------------)

GstGhostPad
(------------------)   -\
| GstPad           |    |
|------------------|    |
| GstPad *target   |     > GstProxyPad
|------------------|    |
| GstPad *internal |    |
|------------------|   -/
| <private data>   |
(------------------)
```

A `GstGhostPad` (X) is _always_ created together with a `GstProxyPad` (Y).
The internal pad pointers are set to point to eachother. The
`GstProxyPad` pairs have opposite directions, the `GstGhostPad` has the same
direction as the (future) ghosted pad (target).

```
(- X --------)
|            |
| target *   |
|------------|
| internal *----+
(------------)  |
  ^             V
  |  (- Y --------)
  |  |            |
  |  | target *   |
  |  |------------|
  +----* internal |
     (------------)

Which we will abbreviate to:

(- X --------)
|            |
| target *--------->//
(------------)
     |
    (- Y --------)
    | target *----->//
    (------------)
```

The `GstGhostPad` (X) is also set as the parent of the `GstProxyPad` (Y).

The target is a pointer to the internal pads peer. It is an optimisation to
quickly get to the peer of a ghostpad without having to dereference the
internal->peer.

Some use case follow with a description of how the datastructure
is modified.

## Creating a ghostpad with a target:

```
gst_ghost_pad_new (char *name, GstPad *target)
```

1) create new GstGhostPad X + GstProxyPad Y
2) X name set to @name
3) X direction is the same as the target, Y is opposite.
4) the target of X is set to @target
5) Y is linked to @target
6) link/unlink and activate functions are set up
   on GstGhostPad.

```
                                (--------------
  (- X --------)                |
  |            |                |------)
  | target *------------------> | sink |
  (------------)       -------> |------)
       |              /         (--------------
      (- Y --------) / (pad link)
//<-----* target   |/
      (------------)
```

- Automatically takes same direction as target.
- target is filled in automatically.

## Creating a ghostpad without a target

```
gst_ghost_pad_new_no_target (char *name, GstPadDirection dir)
```

1) create new GstGhostPad X + GstProxyPad Y
2) X name set to @name
3) X direction is @dir
5) link/unlink and activate functions are set up on GstGhostPad.

```
(- X --------)
|            |
| target *--------->//
(------------)
     |
    (- Y --------)
    | target *----->//
    (------------)
```

- allows for setting the target later

## Setting target on an untargetted unlinked ghostpad

```
gst_ghost_pad_set_target (char *name, GstPad *newtarget)

(- X --------)
|            |
| target *--------->//
(------------)
     |
    (- Y --------)
    | target *----->//
    (------------)
```

1) assert direction of newtarget == X direction
2) target is set to newtarget
3) internal pad Y is linked to newtarget

```
                                (--------------
  (- X --------)                |
  |            |                |------)
  | target *------------------> | sink |
  (------------)       -------> |------)
       |              /         (--------------
      (- Y --------) / (pad link)
//<-----* target   |/
      (------------)
```

## Setting target on a targetted unlinked ghostpad

```
gst_ghost_pad_set_target (char *name, GstPad *newtarget)

                                (--------------
  (- X --------)                |
  |            |                |-------)
  | target *------------------> | sink1 |
  (------------)       -------> |-------)
       |              /         (--------------
      (- Y --------) / (pad link)
//<-----* target   |/
      (------------)
```

1) assert direction of newtarget (sink2) == X direction
2) unlink internal pad Y and oldtarget
3) target is set to newtarget (sink2)
4) internal pad Y is linked to newtarget

```
                                (--------------
  (- X --------)                |
  |            |                |-------)
  | target *------------------> | sink2 |
  (------------)       -------> |-------)
       |              /         (--------------
      (- Y --------) / (pad link)
//<-----* target   |/
      (------------)
```

- Linking a pad to an untargetted ghostpad:

```
    gst_pad_link (src, X)

         (- X --------)
         |            |
         | target *--------->//
         (------------)
              |
             (- Y --------)
             | target *----->//
             (------------)
-------)
       |
 (-----|
 | src |
 (-----|
-------)
```

X is a sink `GstGhostPad` without a target. The internal `GstProxyPad` Y has
the same direction as the src pad (peer).

1) link function is called
  - Y direction is same as @src
  - Y target is set to @src
  - Y is activated in the same mode as X
  - core makes link from @src to X

        ```
                          (- X --------)
                          |            |
                          | target *----->//
                         >(------------)
        (real pad link) /      |
                       /      (- Y ------)
                      /    -----* target |
           -------)  /    /   (----------)
                  | /    /
            (-----|/    /
            | src |<----
            (-----|
           -------)
        ```

## Linking a pad to a targetted ghostpad:

```
    gst_pad_link (src, X)

                                       (--------
               (- X --------)          |
               |            |          |------)
               | target *------------->| sink |
               (------------)         >|------)
                          |          / (--------
                          |         /
                          |        /
-------)                  |       / (real pad link)
       |            (- Y ------) /
 (-----|            |          |/
 | src |       //<----* target |
 (-----|            (----------)
-------)
```

1) link function is called
  - Y direction is same as @src
  - Y target is set to @src
  - Y is activated in the same mode as X
  - core makes link from @src to X

```
                                          (--------
                  (- X --------)          |
                  |            |          |------)
                  | target *------------->| sink |
                 >(------------)         >|------)
(real pad link) /            |          / (--------
               /             |         /
              /              |        /
   -------)  /               |       / (real pad link)
          | /          (- Y ------) /
    (-----|/           |          |/
    | src |<-------------* target |
    (-----|            (----------)
   -------)
```

## Setting target on untargetted linked ghostpad:

```
            gst_ghost_pad_set_target (char *name, GstPad *newtarget)

                  (- X --------)
                  |            |
                  | target *------>//
                 >(------------)
(real pad link) /            |
               /             |
              /              |
   -------)  /               |
          | /          (- Y ------)
    (-----|/           |          |
    | src |<-------------* target |
    (-----|            (----------)
   -------)
```

1) assert direction of @newtarget == X direction
2) X target is set to @newtarget
3) Y is linked to @newtarget

```
                                          (--------
                  (- X --------)          |
                  |            |          |------)
                  | target *------------->| sink |
                 >(------------)         >|------)
(real pad link) /            |          / (--------
               /             |         /
              /              |        /
   -------)  /               |       / (real pad link)
          | /          (- Y ------) /
    (-----|/           |          |/
    | src |<-------------* target |
    (-----|            (----------)
   -------)
```

## Setting target on targetted linked ghostpad:

```
    gst_ghost_pad_set_target (char *name, GstPad *newtarget)

                                          (--------
                  (- X --------)          |
                  |            |          |-------)
                  | target *------------->| sink1 |
                 >(------------)         >|-------)
(real pad link) /            |          / (--------
               /             |         /
              /              |        /
   -------)  /               |       / (real pad link)
          | /          (- Y ------) /
    (-----|/           |          |/
    | src |<-------------* target |
    (-----|            (----------)
   -------)
```

1) assert direction of @newtarget == X direction
2) Y and X target are unlinked
2) X target is set to @newtarget
3) Y is linked to @newtarget

```
                                          (--------
                  (- X --------)          |
                  |            |          |-------)
                  | target *------------->| sink2 |
                 >(------------)         >|-------)
(real pad link) /            |          / (--------
               /             |         /
              /              |        /
   -------)  /               |       / (real pad link)
          | /          (- Y ------) /
    (-----|/           |          |/
    | src |<-------------* target |
    (-----|            (----------)
   -------)
```

## Activation

Sometimes ghost pads should proxy activation functions. This thingie
attempts to explain how it should work in the different cases.

```
    +---+     +----+                             +----+       +----+
    | A +-----+ B  |                             | C  |-------+ D  |
    +---+     +---=+                             +=---+       +----+
                +--=-----------------------------=-+
                |  +=---+   +----+  +----+  +---=+ |
                |  | a  +---+ b  ====  c +--+ d  | |
                |  +----+   +----+  +----+  +----+ |
                |                                  |
                +----------------------------------+
                state change goes from right to left
       <-----------------------------------------------------------
```

All of the labeled boxes are pads. The dashes (---) show pad links, and
the double-lines (===) are internal connections. The box around a, b, c,
and d is a bin. B and C are ghost pads, and a and d are proxy pads. The
arrow represents the direction of a state change algorithm. Not counting
the bin, there are three elements involved here?????????the parent of D, the
parent of A, and the parent of b and c.

Now, in the state change from READY to PAUSED, assuming the pipeline
does not have a live source, all of the pads will end up activated at
the end. There are 4 possible activation modes:

1) AD and ab in PUSH, cd and CD in PUSH
2) AD and ab in PUSH, cd and CD in PULL
3) AD and ab in PULL, cd and CD in PUSH
4) AD and ab in PULL, cd and CD in PULL

When activating (1), the state change algorithm will first visit the
parent of D and activate D in push mode. Then it visits the bin. The bin
will first change the state of its child before activating its pads.
That means c will be activated in push mode. \[\*\] At this point, d and
C should also be active in push mode, because it could be that
activating c in push mode starts a thread, which starts pushing to pads
which aren???t ready yet. Then b is activated in push mode. Then, the bin
activates C in push mode, which should already be in push mode, so
nothing is done. It then activates B in push mode, which activates b in
push mode, but it???s already there, then activates a in push mode as
well. The order of activating a and b does not matter in this case.
Then, finally, the state change algorithm moves to the parent of A,
activates A in push mode, and dataflow begins.

\[\*\] Not yet implemented.

Activation mode (2) is implausible, so we can ignore it for now. That
leaves us with the rest.

(3) is the same as (1) until you get to activating b. Activating b will
proxy directly to activating a, which will activate B and A as well.
Then when the state change algorithm gets to B and A it sees that they
are already active, so it ignores them.

Similarly in (4), activating D will cause the activation of all of the
rest of the pads, in this order: C d c b a B A. Then when the state
change gets to the other elements they are already active, and in fact
data flow is already occurring.

So, from these scenarios, we can distill how ghost pad activation
functions should work:

Ghost source pads (e.g. C): push: called by: element state change
handler behavior: just return TRUE pull: called by: peer???s activatepull
behavior: change the internal pad, which proxies to its peer e.g. C
changes d which changes c.

Internal sink pads (e.g. d): push: called by: nobody (doesn???t seem
possible) behavior: n/a pull: called by: ghost pad behavior: proxy to
peer first

Internal src pads (e.g. a): push: called by: ghost pad behavior:
activate peer in push mode pull: called by: peer???s activatepull
behavior: proxy to ghost pad, which proxies to its peer (e.g. a calls B
which calls A)

Ghost sink pads (e.g. B): push: called by: element state change handler
behavior: change the internal pad, which proxies to peer (e.g. B changes
a which changes b) pull: called by: internal pad behavior: proxy to peer

It doesn???t really make sense to have activation functions on proxy pads
that aren???t part of a ghost pad arrangement.
