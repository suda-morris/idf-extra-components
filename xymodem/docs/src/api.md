# API Reference

## Header files

- [include/xymodem.h](#file-includexymodemh)
- [include/xymodem_core.h](#file-includexymodem_coreh)

## File include/xymodem.h





## Structures and Types

| Type | Name |
| ---: | :--- |
| struct | [**xymodem\_config\_t**](#struct-xymodem_config_t) <br>_Configuration for_ [_**xymodem\_new()**_](#function-xymodem_new)_._ |
| typedef esp\_err\_t(\*) | [**xymodem\_data\_sink\_fn\_t**](#typedef-xymodem_data_sink_fn_t)  <br>_Data sink: hand one accepted data block's payload to the application._ |
| typedef esp\_err\_t(\*) | [**xymodem\_data\_source\_fn\_t**](#typedef-xymodem_data_source_fn_t)  <br>_Data source: pull the next chunk of payload to transmit (sender role only)._ |
| typedef esp\_err\_t(\*) | [**xymodem\_emit\_fn\_t**](#typedef-xymodem_emit_fn_t)  <br>_Push protocol bytes onto the physical link (UART TX, ...)._ |
| typedef void(\*) | [**xymodem\_event\_fn\_t**](#typedef-xymodem_event_fn_t)  <br>_Lifecycle notification (optional)._ |
| struct | [**xymodem\_file\_info\_t**](#struct-xymodem_file_info_t) <br>_File metadata for_ [_**xymodem\_start\_send()**_](#function-xymodem_start_send)_._ |
| typedef struct xymodem\_dev\_t \* | [**xymodem\_handle\_t**](#typedef-xymodem_handle_t)  <br>_Event-driven XMODEM driver handle._ |
| typedef bool(\*) | [**xymodem\_on\_file\_fn\_t**](#typedef-xymodem_on_file_fn_t)  <br>_A block 0 (file header) was verified on receive._ |

## Functions

| Type | Name |
| ---: | :--- |
|  esp\_err\_t | [**xymodem\_cancel**](#function-xymodem_cancel) ([**xymodem\_handle\_t**](#typedef-xymodem_handle_t) handle) <br>_Abort a transfer from the application side._ |
|  esp\_err\_t | [**xymodem\_delete**](#function-xymodem_delete) ([**xymodem\_handle\_t**](#typedef-xymodem_handle_t) handle) <br> |
|  [**xymodem\_err\_t**](#enum-xymodem_err_t) | [**xymodem\_error**](#function-xymodem_error) ([**xymodem\_handle\_t**](#typedef-xymodem_handle_t) handle) <br>_Failure reason (XYMODEM\_ERR\_\*) when failed, XYMODEM\_OK otherwise. XYMODEM\_ERR\_INVALID\_ARG for a NULL / deleted handle._ |
|  esp\_err\_t | [**xymodem\_new**](#function-xymodem_new) (const [**xymodem\_config\_t**](#struct-xymodem_config_t) \*config, [**xymodem\_handle\_t**](#typedef-xymodem_handle_t) \*out\_handle) <br>_Create a driver instance. The handle is reusable for many transfers._ |
|  esp\_err\_t | [**xymodem\_on\_receive**](#function-xymodem_on_receive) ([**xymodem\_handle\_t**](#typedef-xymodem_handle_t) handle, const uint8\_t \*data, size\_t len) <br>_Feed bytes received from the hardware into the protocol state machine._ |
|  esp\_err\_t | [**xymodem\_poll**](#function-xymodem_poll) ([**xymodem\_handle\_t**](#typedef-xymodem_handle_t) handle) <br>_Drive the protocol state machine (timeouts, retransmits)._ |
|  esp\_err\_t | [**xymodem\_start\_recv**](#function-xymodem_start_recv) ([**xymodem\_handle\_t**](#typedef-xymodem_handle_t) handle) <br>_Start a session as the receiving side._ |
|  esp\_err\_t | [**xymodem\_start\_send**](#function-xymodem_start_send) ([**xymodem\_handle\_t**](#typedef-xymodem_handle_t) handle, const [**xymodem\_file\_info\_t**](#struct-xymodem_file_info_t) \*file) <br>_Start a session as the sending side._ |
|  [**xymodem\_status\_t**](#enum-xymodem_status_t) | [**xymodem\_status**](#function-xymodem_status) ([**xymodem\_handle\_t**](#typedef-xymodem_handle_t) handle) <br>_Current transfer status (XYMODEM\_ST\_\*)._ |

## Macros

| Type | Name |
| ---: | :--- |
| define  | [**XYMODEM\_CORE\_CONFIG\_DEFAULT**](#define-xymodem_core_config_default) () <br> |
| define  | [**XYMODEM\_RX\_BUF\_DEFAULT**](#define-xymodem_rx_buf_default)  4096<br> |

## Structures and Types Documentation

### struct `xymodem_config_t`

_Configuration for_ [_**xymodem\_new()**_](#function-xymodem_new)_._

The driver is deliberately transport-agnostic: it never calls a blocking recv(). Feed bytes with [**xymodem\_on\_receive()**](#function-xymodem_on_receive):

* From a UHCI / UART DMA ISR: copies the DMA buffer into an ingest ring. That copy is the one you would do anyway before the descriptor is recycled; the protocol runs later in [**xymodem\_poll()**](#function-xymodem_poll).
* From a task (uart\_read\_bytes, a unit-test buffer): feeds the slice in place, no extra copy. emit / data\_sink may run before it returns.

The I/O callbacks form a 2x2 matrix over two planes and two directions:  wire bytes[**xymodem\_on\_receive()**](#function-xymodem_on_receive)[**emit()**](#variable-emit) payload[**data\_sink()**](#variable-data_sink)[**data\_source()**](#variable-data_source)

"wire" bytes are framed protocol traffic (SOH/STX, block numbers, CRC); "payload" is the file data itself, which the engine strips from (receive) or wraps into (send) that framing.

XMODEM and YMODEM are not two modes: one session is classic XMODEM plus one optional block-0 file header. The receiver recognises a header-ful sender automatically (and accepts classic header-less senders just as well); the sender opts into a header by passing [**xymodem\_file\_info\_t**](#struct-xymodem_file_info_t) to[**xymodem\_start\_send()**](#function-xymodem_start_send). No protocol field, no protocol-specific callbacks.

Every callback's first argument is the handle — the instance, the `this` of this object — then user\_ctx. A UART emit that ignores the handle today still receives it, so a later getter or a shared callback for several instances does not need a new signature. Do not call back into the driver from a callback: the instance lock is held.

The callbacks declare what the application CAN do; the direction of each session is chosen explicitly when it starts:

* [**xymodem\_start\_recv()**](#function-xymodem_start_recv) requires data\_sink: it emits the handshake request immediately and hands every accepted block's payload to it.
* [**xymodem\_start\_send()**](#function-xymodem_start_send) requires data\_source: it waits for the peer's request byte, then pulls payload from it block by block. Sessions are independent on the wire (block numbering restarts at 1), so an echo server simply alternates the two calls; a device that only ever receives can omit data\_source altogether.

Variables:

-  [**xymodem\_core\_config\_t**](#struct-xymodem_core_config_t) core  <br>Protocol tuning (check mode, timeouts).

-  [**xymodem\_data\_sink\_fn\_t**](#typedef-xymodem_data_sink_fn_t) data_sink  <br>Receive path; at least one of data\_sink/data\_source is required.

-  [**xymodem\_data\_source\_fn\_t**](#typedef-xymodem_data_source_fn_t) data_source  <br>Send path; at least one of data\_sink/data\_source is required.

-  [**xymodem\_emit\_fn\_t**](#typedef-xymodem_emit_fn_t) emit  <br>Required: push bytes onto the wire.

-  [**xymodem\_event\_fn\_t**](#typedef-xymodem_event_fn_t) on_event  <br>Optional lifecycle callback (may be NULL).

-  [**xymodem\_on\_file\_fn\_t**](#typedef-xymodem_on_file_fn_t) on_file  <br>Receive path: block 0 accept/reject (may be NULL).

-  size\_t rx_buf_size  <br>Ingest ring for the ISR path of [**xymodem\_on\_receive()**](#function-xymodem_on_receive). 0 = 4096.

-  void \* user_ctx  <br>Opaque pointer passed to every callback.

### typedef `xymodem_data_sink_fn_t`

_Data sink: hand one accepted data block's payload to the application._
```c
typedef esp_err_t(*) xymodem_data_sink_fn_t(xymodem_handle_t handle, void *ctx, const uint8_t *data, size_t len);
```


Called only for blocks that passed the checksum/CRC check and were not duplicates. XMODEM cannot distinguish payload from trailing 0x1A padding, so the whole block (padding included) is handed over, matching classic XMODEM semantics. When the peer sent a file size, payload is trimmed to exactly that size instead.



**Parameters:**


* `handle` The instance (`this`).
* `ctx` User context. 
* `data` Payload bytes (valid only during the call). 
* `len` Number of bytes. 


**Returns:**

ESP\_OK on success; any other value aborts the transfer with XYMODEM\_ERR\_IO.
### typedef `xymodem_data_source_fn_t`

_Data source: pull the next chunk of payload to transmit (sender role only)._
```c
typedef esp_err_t(*) xymodem_data_source_fn_t(xymodem_handle_t handle, void *ctx, uint8_t *buf, size_t len, size_t *read_len);
```


**Parameters:**


* `handle` The instance (`this`).
* `ctx` User context. 
* `buf` Buffer to fill. 
* `len` Capacity of buf. 
* `read_len` Number of bytes actually written (0 = end of data). 


**Returns:**

ESP\_OK on success; any other value aborts the transfer with XYMODEM\_ERR\_IO.
### typedef `xymodem_emit_fn_t`

_Push protocol bytes onto the physical link (UART TX, ...)._
```c
typedef esp_err_t(*) xymodem_emit_fn_t(xymodem_handle_t handle, void *ctx, const uint8_t *data, size_t len);
```


Called from [**xymodem\_on\_receive()**](#function-xymodem_on_receive) (task path),[**xymodem\_poll()**](#function-xymodem_poll), or xymodem\_start\_\*() — always in task context, never from the ISR path of on\_receive. Must not block for a response; only queue/transmit the bytes. Must not call back into the driver: the instance lock is held.



**Parameters:**


* `handle` The instance (`this`). Same pointer as every public API.
* `ctx` User context from [**xymodem\_config\_t::user\_ctx**](#variable-user_ctx).
* `data` Bytes to transmit. 
* `len` Number of bytes. 


**Returns:**

ESP\_OK on success; any other value aborts the transfer with XYMODEM\_ERR\_IO (both the event callback and [**xymodem\_error()**](#function-xymodem_error) report that same code) and emits CAN to the peer.
### typedef `xymodem_event_fn_t`

_Lifecycle notification (optional)._
```c
typedef void(*) xymodem_event_fn_t(xymodem_handle_t handle, void *ctx, xymodem_event_t event, xymodem_err_t arg);
```


**Parameters:**


* `handle` The instance (`this`) reporting the event.
* `ctx` User context from [**xymodem\_config\_t::user\_ctx**](#variable-user_ctx).
* `event` One of XYMODEM\_EV\_\*. XYMODEM\_EV\_FILE\_COMPLETE fires at each YMODEM file EOT (the session stays open for the next header); XYMODEM\_EV\_COMPLETE marks the whole batch / classic transfer; XYMODEM\_EV\_FAILED carries the reason in `arg`. 
* `arg` On XYMODEM\_EV\_FAILED, a negative XYMODEM\_ERR\_\* code; 0 otherwise.
### struct `xymodem_file_info_t`

_File metadata for_ [_**xymodem\_start\_send()**_](#function-xymodem_start_send)_._

The name is copied by the driver; the caller's buffer may be transient. It must be NUL-terminated and, when non-empty, fit a 128-byte header block together with the decimal size; the engine validates this and returns ESP\_ERR\_INVALID\_ARG if it does not. An empty name sends the empty block 0 that terminates a YMODEM batch (no payload) — that empty pathname is the YMODEM wire token, not a separate protocol mode.

Variables:

-  const char \* name  <br>File name sent in block 0 (NUL-terminated).

-  uint64\_t size  <br>Exact file size in bytes; 0 sends an empty file. uint64\_t matches [**xymodem\_on\_file\_fn\_t**](#typedef-xymodem_on_file_fn_t) and the wire header, so sizes &gt; 4 GiB survive a 32-bit build.

### typedef `xymodem_handle_t`

_Event-driven XMODEM driver handle._
```c
typedef struct xymodem_dev_t* xymodem_handle_t;
```


The driver owns no hardware. It wraps the pure Rust protocol state machine and drives it from the caller's task; the caller supplies the physical link and the payload through callbacks. See [**xymodem\_config\_t**](#struct-xymodem_config_t).
### typedef `xymodem_on_file_fn_t`

_A block 0 (file header) was verified on receive._
```c
typedef bool(*) xymodem_on_file_fn_t(xymodem_handle_t handle, void *ctx, const char *name, uint64_t size, bool has_size);
```


Called before any payload is delivered, so this is the place to open the target file, check the size against your storage, or refuse unknown files. Return false to reject: the transfer is cancelled with CAN and reported as XYMODEM\_ERR\_FILE\_REJECTED (the file was refused, not an I/O failure). Classic (header-less) senders never emit block 0, so this is simply never called for them.



**Parameters:**


* `handle` The instance (`this`).
* `ctx` User context. 
* `name` File name (NUL-terminated; valid only during the call). 
* `size` Declared file size; meaningless when has\_size is false. 
* `has_size` Whether the header carried a size field. Without one, payload is delivered whole (classic semantics, SUB padding included) and the exact file length is unknown. 


**Returns:**

true to accept the file; false rejects it and cancels the transfer.

## Functions Documentation

### function `xymodem_cancel`

_Abort a transfer from the application side._
```c
esp_err_t xymodem_cancel (
    xymodem_handle_t handle
) 
```


Emits CAN bytes to the peer and reports XYMODEM\_ERR\_CANCEL\_BY\_LOCAL through the event callback and [**xymodem\_error()**](#function-xymodem_error). A peer-initiated cancel is reported as XYMODEM\_ERR\_CANCEL\_BY\_PEER instead. No-op once the session is terminal.
### function `xymodem_delete`

```c
esp_err_t xymodem_delete (
    xymodem_handle_t handle
) 
```


Tear down a driver instance and release its memory.
### function `xymodem_error`

_Failure reason (XYMODEM\_ERR\_\*) when failed, XYMODEM\_OK otherwise. XYMODEM\_ERR\_INVALID\_ARG for a NULL / deleted handle._
```c
xymodem_err_t xymodem_error (
    xymodem_handle_t handle
) 
```

### function `xymodem_new`

_Create a driver instance. The handle is reusable for many transfers._
```c
esp_err_t xymodem_new (
    const xymodem_config_t *config,
    xymodem_handle_t *out_handle
) 
```


The driver starts idle: nothing is emitted and no timer runs until the first [**xymodem\_start\_recv()**](#function-xymodem_start_recv) /[**xymodem\_start\_send()**](#function-xymodem_start_send). A session that completed, failed or was cancelled can simply be followed by the next one. Create one handle at init and keep it for the lifetime of your task.



**Parameters:**


* `config` Driver configuration (copied by value). 
* `out_handle` Returned handle. 


**Returns:**



* ESP\_OK on success.
* ESP\_ERR\_INVALID\_ARG for missing/NULL callbacks or a bad config.
* ESP\_ERR\_NO\_MEM if the handle cannot be allocated.
### function `xymodem_on_receive`

_Feed bytes received from the hardware into the protocol state machine._
```c
esp_err_t xymodem_on_receive (
    xymodem_handle_t handle,
    const uint8_t *data,
    size_t len
) 
```


Call this from either context; the copy happens only when it has to:



* UHCI / UART DMA ISR: copies `data` into an ingest ring (spinlock, no mutex, no callbacks). This is the copy-out-of-DMA you would do before the descriptor is recycled. Protocol work runs in[**xymodem\_poll()**](#function-xymodem_poll). A full ring returns ESP\_ERR\_NO\_MEM (raise rx\_buf\_size or poll more often).
* Task (`uart_read_bytes`, a unit-test buffer): feeds the caller's slice in place. The UART FIFO driver already copied out of hardware; this path does not copy again. emit / data\_sink may run before return.

Any slice is valid  half a packet, several packets, a stray ACK mixed with data. Time is taken from esp\_timer, so no clock is passed in.



**Parameters:**


* `handle` Driver handle. 
* `data` Received bytes (may be NULL when len == 0). 
* `len` Number of bytes. 


**Returns:**

ESP\_OK, ESP\_ERR\_INVALID\_ARG, or ESP\_ERR\_NO\_MEM (ISR path, ring full).
### function `xymodem_poll`

_Drive the protocol state machine (timeouts, retransmits)._
```c
esp_err_t xymodem_poll (
    xymodem_handle_t handle
) 
```


Call from the same loop that reads the UART (the read timeout _is_ the poll period), never from an ISR. Drains bytes ingested from an ISR, then advances timers. A UHCI-only setup needs no RX task: the ISR calls on\_receive, this call runs the protocol.



**Parameters:**


* `handle` Driver handle. 


**Returns:**

ESP\_OK, or ESP\_ERR\_INVALID\_ARG for a NULL handle.
### function `xymodem_start_recv`

_Start a session as the receiving side._
```c
esp_err_t xymodem_start_recv (
    xymodem_handle_t handle
) 
```


Requires [**xymodem\_config\_t::data\_sink**](#variable-data_sink). Resets the protocol state, the block counter and the negotiated check mode, then emits the handshake request ('C' or NAK) right away; feed received bytes in with[**xymodem\_on\_receive()**](#function-xymodem_on_receive). A peer that sends block 0 (file name/size) is recognised automatically; a classic header-less sender is detected from its first data block. The application's own buffers are untouched reset those yourself.

Typical echo server (see examples/xymodem\_event). All start calls happen in the task loop, never inside a callback: callbacks fire with the driver lock held, so calling back into xymodem\_start\_\*() from on\_event would deadlock on the non-recursive mutex. 

A session already in flight is NOT silently abandoned: the call returns ESP\_ERR\_INVALID\_STATE. Wait for XYMODEM\_EV\_COMPLETE / XYMODEM\_EV\_FAILED (or poll [**xymodem\_status()**](#function-xymodem_status)) before starting the next one.



**Parameters:**


* `handle` Driver handle. 


**Returns:**

ESP\_OK, ESP\_ERR\_INVALID\_ARG for a NULL handle, or ESP\_ERR\_INVALID\_STATE when no data\_sink callback was configured or a session is still in flight.
### function `xymodem_start_send`

_Start a session as the sending side._
```c
esp_err_t xymodem_start_send (
    xymodem_handle_t handle,
    const xymodem_file_info_t *file
) 
```


Requires [**xymodem\_config\_t::data\_source**](#variable-data_source). Resets the protocol state, the block counter and the negotiated check mode, then waits for the peer's request byte (XMODEM lets the receiver talk first); payload is then pulled from data\_source block by block.

`file` selects the session's optional block 0:

* NULL: classic XMODEM, header-less (no file name/size on the wire).
* non-NULL: block 0 carries file-&gt;name and file-&gt;size first (YMODEM); payload is then pulled from data\_source. An empty file-&gt;name sends the empty block 0 that terminates a YMODEM batch, with no payload.

A YMODEM batch (several files over one wire session) is just several [**xymodem\_start\_send()**](#function-xymodem_start_send) calls with one file each, followed by one empty-name terminator. The library keeps no batch bookkeeping: each call waits for the receiver's fresh 'C', which is exactly the YMODEM batch wire timing. Call[**xymodem\_start\_send()**](#function-xymodem_start_send) again only after the previous session completed.

As with [**xymodem\_start\_recv()**](#function-xymodem_start_recv), starting on top of an in-flight session is rejected with ESP\_ERR\_INVALID\_STATE rather than silently dropping it.



**Parameters:**


* `handle` Driver handle. 
* `file` Optional file metadata (NULL = classic XMODEM). 


**Returns:**

ESP\_OK, ESP\_ERR\_INVALID\_ARG for a NULL handle or a name that does not fit the header block, ESP\_ERR\_INVALID\_STATE when no data\_source callback was configured or a session is still in flight.
### function `xymodem_status`

_Current transfer status (XYMODEM\_ST\_\*)._
```c
xymodem_status_t xymodem_status (
    xymodem_handle_t handle
) 
```


**Returns:**

A status code, or XYMODEM\_ST\_INVALID for a NULL / deleted handle.

## Macros Documentation

### define `XYMODEM_CORE_CONFIG_DEFAULT`

```c
#define XYMODEM_CORE_CONFIG_DEFAULT (
    
) { \
    .check_mode = XYMODEM_CHECK_AUTO , \
    .block_1k = false, \
    .max_retries = 10, \
    .packet_timeout_ms = 3000, \
    .response_timeout_ms = 30000, \
}
```


Default core tuning: CRC-auto (three 'C's then SUM8), 128-byte blocks, 10 retries, 3s/30s timeouts. `block_1k` senders drop the last block to SOH/128 when it is at most 128 bytes.
### define `XYMODEM_RX_BUF_DEFAULT`

```c
#define XYMODEM_RX_BUF_DEFAULT 4096
```


Default [**xymodem\_config\_t::rx\_buf\_size**](#variable-rx_buf_size) when that field is 0.

## File include/xymodem_core.h





## Structures and Types

| Type | Name |
| ---: | :--- |
| enum  | [**xymodem\_check\_mode\_t**](#enum-xymodem_check_mode_t)  <br> |
| typedef uint8\_t | [**xymodem\_check\_mode\_t**](#typedef-xymodem_check_mode_t)  <br> |
| struct | [**xymodem\_core\_config\_t**](#struct-xymodem_core_config_t) <br> |
| typedef struct xymodem\_core\_config\_t | [**xymodem\_core\_config\_t**](#typedef-xymodem_core_config_t)  <br> |
| struct | [**xymodem\_core\_file\_info\_t**](#struct-xymodem_core_file_info_t) <br> |
| typedef struct xymodem\_core\_file\_info\_t | [**xymodem\_core\_file\_info\_t**](#typedef-xymodem_core_file_info_t)  <br> |
| struct | [**xymodem\_core\_io\_t**](#struct-xymodem_core_io_t) <br> |
| typedef struct xymodem\_core\_io\_t | [**xymodem\_core\_io\_t**](#typedef-xymodem_core_io_t)  <br> |
| struct | [**xymodem\_core\_t**](#struct-xymodem_core_t) <br> |
| typedef struct xymodem\_core\_t | [**xymodem\_core\_t**](#typedef-xymodem_core_t)  <br> |
| enum  | [**xymodem\_err\_t**](#enum-xymodem_err_t)  <br> |
| typedef int32\_t | [**xymodem\_err\_t**](#typedef-xymodem_err_t)  <br> |
| enum  | [**xymodem\_event\_t**](#enum-xymodem_event_t)  <br> |
| typedef uint32\_t | [**xymodem\_event\_t**](#typedef-xymodem_event_t)  <br> |
| enum  | [**xymodem\_status\_t**](#enum-xymodem_status_t)  <br> |
| typedef int32\_t | [**xymodem\_status\_t**](#typedef-xymodem_status_t)  <br> |

## Functions

| Type | Name |
| ---: | :--- |
|  [**xymodem\_err\_t**](#enum-xymodem_err_t) | [**xymodem\_core\_cancel**](#function-xymodem_core_cancel) (struct [**xymodem\_core\_t**](#struct-xymodem_core_t) \*core) <br> |
|  [**xymodem\_err\_t**](#enum-xymodem_err_t) | [**xymodem\_core\_error**](#function-xymodem_core_error) (const struct [**xymodem\_core\_t**](#struct-xymodem_core_t) \*core) <br> |
|  [**xymodem\_err\_t**](#enum-xymodem_err_t) | [**xymodem\_core\_init**](#function-xymodem_core_init) (struct [**xymodem\_core\_t**](#struct-xymodem_core_t) \*core, const struct [**xymodem\_core\_config\_t**](#struct-xymodem_core_config_t) \*config, const struct [**xymodem\_core\_io\_t**](#struct-xymodem_core_io_t) \*io) <br> |
|  [**xymodem\_err\_t**](#enum-xymodem_err_t) | [**xymodem\_core\_on\_receive**](#function-xymodem_core_on_receive) (struct [**xymodem\_core\_t**](#struct-xymodem_core_t) \*core, const uint8\_t \*data, size\_t len, uint32\_t now\_ms) <br> |
|  [**xymodem\_err\_t**](#enum-xymodem_err_t) | [**xymodem\_core\_poll**](#function-xymodem_core_poll) (struct [**xymodem\_core\_t**](#struct-xymodem_core_t) \*core, uint32\_t now\_ms) <br> |
|  [**xymodem\_err\_t**](#enum-xymodem_err_t) | [**xymodem\_core\_start\_recv**](#function-xymodem_core_start_recv) (struct [**xymodem\_core\_t**](#struct-xymodem_core_t) \*core, uint32\_t now\_ms) <br> |
|  [**xymodem\_err\_t**](#enum-xymodem_err_t) | [**xymodem\_core\_start\_send**](#function-xymodem_core_start_send) (struct [**xymodem\_core\_t**](#struct-xymodem_core_t) \*core, const struct [**xymodem\_core\_file\_info\_t**](#struct-xymodem_core_file_info_t) \*file, uint32\_t now\_ms) <br> |
|  [**xymodem\_status\_t**](#enum-xymodem_status_t) | [**xymodem\_core\_status**](#function-xymodem_core_status) (const struct [**xymodem\_core\_t**](#struct-xymodem_core_t) \*core) <br> |

## Macros

| Type | Name |
| ---: | :--- |
| define  | [**XYMODEM\_CORE\_STATE\_WORDS**](#define-xymodem_core_state_words)  280<br> |

## Structures and Types Documentation

### enum `xymodem_check_mode_t`

```c
enum xymodem_check_mode_t {
    XYMODEM_CHECK_AUTO = 0,
    XYMODEM_CHECK_SUM8 = 1,
    XYMODEM_CHECK_CRC16 = 2
};
```

### typedef `xymodem_check_mode_t`

```c
typedef uint8_t xymodem_check_mode_t;
```

### struct `xymodem_core_config_t`


Variables:

-  bool block_1k  

-  uint8\_t check_mode  

-  uint16\_t max_retries  

-  uint32\_t packet_timeout_ms  

-  uint32\_t response_timeout_ms  

### typedef `xymodem_core_config_t`

```c
typedef struct xymodem_core_config_t xymodem_core_config_t;
```

### struct `xymodem_core_file_info_t`


Variables:

-  const char \* name  

-  uint64\_t size  

### typedef `xymodem_core_file_info_t`

```c
typedef struct xymodem_core_file_info_t xymodem_core_file_info_t;
```

### struct `xymodem_core_io_t`


Variables:

-  int(\*) data_sink  

-  int(\*) data_source  

-  int(\*) emit  

-  void(\*) on_event  

-  bool(\*) on_file  

-  void \* user  

### typedef `xymodem_core_io_t`

```c
typedef struct xymodem_core_io_t xymodem_core_io_t;
```

### struct `xymodem_core_t`


Variables:

-  uint64\_t _storage  

### typedef `xymodem_core_t`

```c
typedef struct xymodem_core_t xymodem_core_t;
```

### enum `xymodem_err_t`

```c
enum xymodem_err_t {
    XYMODEM_OK = 0,
    XYMODEM_ERR_INVALID_ARG = -1,
    XYMODEM_ERR_CANCEL_BY_PEER = -2,
    XYMODEM_ERR_NO_RESPONSE = -3,
    XYMODEM_ERR_RETRY_EXCEEDED = -4,
    XYMODEM_ERR_OUT_OF_SYNC = -5,
    XYMODEM_ERR_IO = -6,
    XYMODEM_ERR_FILE_REJECTED = -7,
    XYMODEM_ERR_CANCEL_BY_LOCAL = -8,
    XYMODEM_ERR_BUSY = -9
};
```

### typedef `xymodem_err_t`

```c
typedef int32_t xymodem_err_t;
```

### enum `xymodem_event_t`

```c
enum xymodem_event_t {
    XYMODEM_EV_COMPLETE = 3,
    XYMODEM_EV_FAILED = 4,
    XYMODEM_EV_FILE_COMPLETE = 5
};
```

### typedef `xymodem_event_t`

```c
typedef uint32_t xymodem_event_t;
```

### enum `xymodem_status_t`

```c
enum xymodem_status_t {
    XYMODEM_ST_IDLE = 0,
    XYMODEM_ST_HANDSHAKE = 1,
    XYMODEM_ST_TRANSFER = 2,
    XYMODEM_ST_COMPLETE = 3,
    XYMODEM_ST_FAILED = 4,
    XYMODEM_ST_INVALID = 5
};
```

### typedef `xymodem_status_t`

```c
typedef int32_t xymodem_status_t;
```


## Functions Documentation

### function `xymodem_core_cancel`

```c
xymodem_err_t xymodem_core_cancel (
    struct xymodem_core_t *core
) 
```

### function `xymodem_core_error`

```c
xymodem_err_t xymodem_core_error (
    const struct xymodem_core_t *core
) 
```

### function `xymodem_core_init`

```c
xymodem_err_t xymodem_core_init (
    struct xymodem_core_t *core,
    const struct xymodem_core_config_t *config,
    const struct xymodem_core_io_t *io
) 
```

### function `xymodem_core_on_receive`

```c
xymodem_err_t xymodem_core_on_receive (
    struct xymodem_core_t *core,
    const uint8_t *data,
    size_t len,
    uint32_t now_ms
) 
```

### function `xymodem_core_poll`

```c
xymodem_err_t xymodem_core_poll (
    struct xymodem_core_t *core,
    uint32_t now_ms
) 
```

### function `xymodem_core_start_recv`

```c
xymodem_err_t xymodem_core_start_recv (
    struct xymodem_core_t *core,
    uint32_t now_ms
) 
```

### function `xymodem_core_start_send`

```c
xymodem_err_t xymodem_core_start_send (
    struct xymodem_core_t *core,
    const struct xymodem_core_file_info_t *file,
    uint32_t now_ms
) 
```

### function `xymodem_core_status`

```c
xymodem_status_t xymodem_core_status (
    const struct xymodem_core_t *core
) 
```


## Macros Documentation

### define `XYMODEM_CORE_STATE_WORDS`

```c
#define XYMODEM_CORE_STATE_WORDS 280
```


