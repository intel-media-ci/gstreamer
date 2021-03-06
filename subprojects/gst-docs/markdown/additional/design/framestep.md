# Frame stepping

This document outlines the details of the frame stepping functionality
in GStreamer.

The stepping functionality operates on the current playback segment,
position and rate as it was configured with a regular seek event. In
contrast to the seek event, it operates very closely to the sink and
thus has a very low latency and is not slowed down by queues and does
not actually perform any seeking logic. For this reason we want to
include a new API instead of reusing the seek API.

The following requirements are needed:

- The ability to walk forwards and backwards in the stream.

- Arbitrary increments in any supported format (time, frames, bytes …)

- High speed, minimal overhead. This mechanism is not more expensive
than simple playback.

- switching between forwards and backwards stepping should be fast.

- Maintain synchronisation between streams.

- Get feedback of the amount of skipped data.

- Ability to play a certain amount of data at an arbitrary speed.

We want a system where we can step frames in `PAUSED` as well as play
short segments of data in `PLAYING`.

## Use Cases

### video only pipeline in PAUSED

```
.-----.    .-------.              .------.    .-------.
| src |    | demux |    .-----.   | vdec |    | vsink |
|    src->sink    src1->|queue|->sink   src->sink     |
'-----'    '-------'    '-----'   '------'    '-------'
```

  - app sets the pipeline to `PAUSED` to block on the preroll picture

  - app seeks to required position in the stream. This can be done
    with a positive or negative rate depending on the required frame
    stepping direction.

  - app steps frames (in `GST_FORMAT_DEFAULT` or `GST_FORMAT_BUFFER)`. The
  pipeline loses its `PAUSED` state until the required number of frames have been
  skipped, it then prerolls again. This skipping is purely done in the sink.

  - sink posts `STEP_DONE` with amount of frames stepped and
    corresponding time interval.

### audio/video pipeline in PAUSED

```
.-----.    .-------.              .------.    .-------.
| src |    | demux |    .-----.   | vdec |    | vsink |
|    src->sink    src1->|queue|->sink   src->sink     |
'-----'    |       |    '-----'   '------'    '-------'
           |       |              .------.    .-------.
           |       |    .-----.   | adec |    | asink |
           |      src2->|queue|->sink   src->sink     |
           '-------'    '-----'   '------'    '-------'
```

  - app sets the pipeline to `PAUSED` to block on the preroll picture

  - app seeks to required position in the stream. This can be done
    with a positive or negative rate depending on the required frame
    stepping direction.

  - app steps frames (in `GST_FORMAT_DEFAULT` or `GST_FORMAT_BUFFER`) or an
  amount of time on the video sink. The pipeline loses its `PAUSED` state until
  the required number of frames have been skipped, it then prerolls again. This
  skipping is purely done in the sink.

  - sink posts `STEP_DONE` with amount of frames stepped and
    corresponding time interval.

  - the app skips the same amount of time on the audiosink to align
    the streams again. When huge amount of video frames are skipped,
    there needs to be enough queueing in the pipeline to compensate
    for the accumulated audio.

### audio/video pipeline in PLAYING

  - app sets the pipeline to `PAUSED` to block on the preroll picture

  - app seeks to required position in the stream. This can be done
    with a positive or negative rate depending on the required frame
    stepping direction.

  - app configures frames steps (in `GST_FORMAT_DEFAULT` or
  `GST_FORMAT_BUFFER` or an amount of time on the sink. The step event has
  a flag indicating live stepping so that the stepping will only happens in
  PLAYING.

  - app sets pipeline to PLAYING. The pipeline continues PLAYING
    until it consumed the amount of time.

  - sink posts `STEP_DONE` with amount of frames stepped and
    corresponding time interval. The sink will then wait for another
    step event. Since the `STEP_DONE` message was emitted by the sink
    when it handed off the buffer to the device, there is usually
    sufficient time to queue a new STEP event so that one can
    seamlessly continue stepping.

## events

A new `GST_EVENT_STEP` event is introduced to start the step operation.
The step event is created with the following fields in the structure:

* **`format`** `GST_TYPE_FORMAT`: The format of the step units

* **`amount`** `G_TYPE_UINT64`: The amount of units to step. A 0 amount
immediately completes and can be used to cancel the current step and resume
normal non-stepping behaviour to the end of the segment. A -1 amount steps
until the end of the segment.

* **`rate`** `G_TYPE_DOUBLE`: The rate at which the frames should be stepped in
PLAYING mode. 1.0 is the normal playback speed and direction of the segment,
2.0 is double speed. A speed of 0.0 is not allowed. When performing a flushing
step, the speed is not relevant. Note that we don't allow negative rates here,
use a seek with a negative rate first to reverse the playback direction.

* **`flush`** `G_TYPE_BOOLEAN`: when flushing is TRUE, the step is performed
immediately:

  - In the `PAUSED` state the pipeline loses the `PAUSED` state, the
    requested amount of data is skipped and the pipeline prerolls again
    when a non-intermediate step completes. When the pipeline was
    stepping while the event is sent, the current step operation is
    updated with the new amount and format. The sink will do a best
    effort to comply with the new amount.

  - In the PLAYING state, the pipeline loses the `PLAYING` state, the
    requested amount of data is skipped (not rendered) from the previous
    STEP request or from the position of the last `PAUSED` if no previous
    STEP operation was performed. The pipeline goes back to the `PLAYING`
    state when a non-intermediate step completes.

  - When flushing is FALSE, the step will be performed later.

  - In the `PAUSED` state the step will be done when going to `PLAYING`. Any
    previous step operation will be overridden with the new `STEP` event.

  - In the `PLAYING` state the step operation will be performed after the
    current step operation completes. If there was no previous step
    operation, the step operation will be performed from the position of
    the last `PAUSED` state.

* **`intermediate`** `G_TYPE_BOOLEAN`: Signal that this step operation is an
intermediate step, part of a series of step operations. It is mostly
interesting for stepping in the `PAUSED` state because the sink will only perform
a preroll after a non-intermediate step operation completes. Intermediate steps
are useful to flush out data from other sinks in order to not cause excessive
queueing. In the PLAYING state the intermediate flag has no visual effect. In
all states, the intermediate flag is passed to the corresponding
`GST_MESSAGE_STEP_DONE`.

The application will create a STEP event to start or stop the stepping
operation. Both stepping in `PAUSED` and `PLAYING` can be performed by means
of the flush flag.

The event is usually sent to the pipeline, which will typically
distribute the event to all of its sinks. For some use cases, like frame
stepping on video frames only, the event should only be sent to the
video sink and upon reception of the `STEP_DONE` message, one can step
the other sinks to align the streams again.

For large stepping amounts, there needs to be enough queueing in front
of all the sinks. If large steps need to be performed, they can be split
up into smaller step operations using the "intermediate" flag on the
step.

Since the step event does not update the `base_time` of any of the
elements, the sinks should keep track of the amount of stepped data in
order to remain synchronized against the clock.

## messages

A `GST_MESSAGE_STEP_START` is created. It contains the following
fields.

* **`active`**: If the step was queued or activated.

* **`format`** `GST_TYPE_FORMAT`: The format of the step units that queued/activated.

* **`amount`** `G_TYPE_UINT64`: The amount of units that were queued/activated.

* **`rate`** `G_TYPE_DOUBLE`: The rate and direction at which the frames were queued/activated.

* **`flush`** `G_TYPE_BOOLEAN`: If the queued/activated frames will be flushed.

* **`intermediate`** `G_TYPE_BOOLEAN`: If this is an intermediate step operation
that queued/activated.

The `STEP_START` message is emitted 2 times:

  - first when an element received the STEP event and queued it. The
    "active" field will be FALSE in this case.

  - second when the step operation started in the streaming thread. The
    "active" field is TRUE in this case. After this message is emitted,
    the application can queue a new step operation.

The purpose of this message is to find out how many elements participate
in the step operation and to queue new step operations at the earliest
possible moment.

A new `GST_MESSAGE_STEP_DONE` message is created. It contains the
following fields:

* **`format`** `GST_TYPE_FORMAT`: The format of the step units that completed.
* **`amount`** `G_TYPE_UINT64`: The amount of units that were stepped.
* **`rate`** `G_TYPE_DOUBLE`: The rate and direction at which the frames were stepped.
* **`flush`** `G_TYPE_BOOLEAN`: If the stepped frames were flushed.
* **`intermediate`** `G_TYPE_BOOLEAN`: If this is an intermediate step operation that completed.
* **`duration`** `G_TYPE_UINT64`: The total duration of the stepped units in `GST_FORMAT_TIME`.
* **`eos`** `G_TYPE_BOOLEAN`: The step ended because of EOS.

The message is emitted by the element that performs the step operation.
The purpose is to return the duration in `GST_FORMAT_TIME` of the
stepped media. This especially interesting to align other stream in case
of stepping frames on the video sink element.

## Direction switch

When quickly switching between a forwards and a backwards step of, for
example, one video frame, we need either:

1) issue a new seek to change the direction from the current position.
2) cache a certain number of stepped frames and walk the cache.

option 1) might be very slow. For option 2) we would ideally like to
offload this caching functionality to a separate element, which means
that we need to forward the STEP event upstream. It’s unclear how this
could work in a generic way. What is a demuxer supposed to do when it
received a step event? a flushing seek to what stream position?
