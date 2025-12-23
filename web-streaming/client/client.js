/**
 * Basilisk II WebRTC Client (libdatachannel backend)
 *
 * Full-featured client with debugging, stats tracking, and connection monitoring.
 */

// Global debug configuration (fetched from server)
const debugConfig = {
    debug_connection: false,   // WebRTC/ICE/signaling logs
    debug_mode_switch: false,  // Mode/resolution/color depth changes
    debug_perf: false          // Performance stats, ping logs
};

// Fetch debug config from server
async function fetchDebugConfig() {
    try {
        const response = await fetch('/api/config');
        const config = await response.json();
        Object.assign(debugConfig, config);
        console.log('[Browser] Debug config:', debugConfig);
    } catch (e) {
        console.warn('[Browser] Failed to fetch debug config, using defaults');
    }
}

// Debug logging system - sends to server and local debug panel
class DebugLogger {
    constructor() {
        this.logElement = null;
        this.maxEntries = 500;
        this.sendToServer = true;  // Send important logs to server
    }

    init() {
        this.logElement = document.getElementById('debug-log');
    }

    log(level, message, data = null) {
        const timestamp = new Date().toISOString().split('T')[1].slice(0, 12);
        const logLine = data ? `${message}: ${JSON.stringify(data)}` : message;

        // Console output with [Browser] prefix
        const consoleFn = level === 'error' ? console.error :
                         level === 'warn' ? console.warn : console.log;
        consoleFn(`[Browser] ${level}: ${logLine}`);

        // UI output
        if (this.logElement) {
            const entry = document.createElement('div');
            entry.className = `log-entry ${level}`;
            entry.innerHTML = `<span class="timestamp">${timestamp}</span>${this.escapeHtml(logLine)}`;
            this.logElement.appendChild(entry);

            // Trim old entries
            while (this.logElement.children.length > this.maxEntries) {
                this.logElement.removeChild(this.logElement.firstChild);
            }

            // Auto-scroll
            this.logElement.scrollTop = this.logElement.scrollHeight;
        }

        // Send to server (errors, warnings, and key info messages)
        if (this.sendToServer && (level === 'error' || level === 'warn' || level === 'info')) {
            this.sendToServerAsync(level, message, data);
        }
    }

    async sendToServerAsync(level, message, data) {
        try {
            await fetch('/api/log', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({
                    level,
                    message,
                    data: data ? JSON.stringify(data) : ''
                })
            });
        } catch (e) {
            // Silently ignore - don't create infinite loops
        }
    }

    escapeHtml(text) {
        const div = document.createElement('div');
        div.textContent = text;
        return div.innerHTML;
    }

    info(msg, data) { this.log('info', msg, data); }
    warn(msg, data) { this.log('warn', msg, data); }
    error(msg, data) { this.log('error', msg, data); }
    debug(msg, data) { this.log('debug', msg, data); }

    clear() {
        if (this.logElement) {
            this.logElement.innerHTML = '';
        }
    }
}

const logger = new DebugLogger();

// Error reporting to server
function reportErrorToServer(error, type = 'error') {
    try {
        const errorData = {
            message: error.message || String(error),
            stack: error.stack || '',
            url: error.filename || window.location.href,
            line: error.lineno || '',
            col: error.colno || '',
            type: type,
            timestamp: new Date().toISOString(),
            userAgent: navigator.userAgent
        };

        // Send to server via beacon (works even during page unload)
        const blob = new Blob([JSON.stringify(errorData)], { type: 'application/json' });
        navigator.sendBeacon('/api/error', blob);
    } catch (e) {
        // Fail silently - don't want error reporting to cause more errors
        console.error('Failed to report error to server:', e);
    }
}

// Global error handler for uncaught exceptions
window.addEventListener('error', (event) => {
    reportErrorToServer({
        message: event.message,
        filename: event.filename,
        lineno: event.lineno,
        colno: event.colno,
        stack: event.error ? event.error.stack : ''
    }, 'UncaughtException');
});

// Global handler for unhandled promise rejections
window.addEventListener('unhandledrejection', (event) => {
    reportErrorToServer({
        message: `Unhandled Promise Rejection: ${event.reason}`,
        stack: event.reason && event.reason.stack ? event.reason.stack : String(event.reason)
    }, 'UnhandledPromiseRejection');
});

// Connection step tracking
class ConnectionSteps {
    constructor() {
        this.steps = ['ws', 'offer', 'ice', 'track', 'frames'];
        this.currentStep = -1;
    }

    reset() {
        this.currentStep = -1;
        this.steps.forEach(step => {
            const el = document.getElementById(`step-${step}`);
            if (el) {
                el.className = 'step';
                el.querySelector('.step-icon').textContent = this.steps.indexOf(step) + 1;
            }
        });
    }

    setActive(stepName) {
        const idx = this.steps.indexOf(stepName);
        if (idx === -1) return;

        this.steps.forEach((step, i) => {
            const el = document.getElementById(`step-${step}`);
            if (!el) return;

            if (i < idx) {
                el.className = 'step done';
                el.querySelector('.step-icon').innerHTML = '&#10003;';
            } else if (i === idx) {
                el.className = 'step active';
                el.querySelector('.step-icon').innerHTML = '<div class="step-spinner"></div>';
            } else {
                el.className = 'step';
                el.querySelector('.step-icon').textContent = i + 1;
            }
        });

        this.currentStep = idx;
    }

    setDone(stepName) {
        const el = document.getElementById(`step-${stepName}`);
        if (el) {
            el.className = 'step done';
            el.querySelector('.step-icon').innerHTML = '&#10003;';
        }
    }

    setError(stepName) {
        const el = document.getElementById(`step-${stepName}`);
        if (el) {
            el.className = 'step error';
            el.querySelector('.step-icon').innerHTML = '&#10007;';
        }
    }
}

const connectionSteps = new ConnectionSteps();


/*
 * Video Decoder Abstraction
 *
 * Allows switching between different decoding strategies:
 * - H.264 via WebRTC video track (native browser decoding)
 * - PNG via DataChannel (good for dithered 1-bit content)
 * - Raw RGBA via DataChannel (lowest latency, highest bandwidth)
 */

const CodecType = {
    H264: 'h264',   // WebRTC video track with H.264
    PNG: 'png',     // PNG over DataChannel
    RAW: 'raw'      // Raw RGBA over DataChannel
};

// Base class for video decoders
class VideoDecoder {
    constructor(displayElement) {
        this.display = displayElement;
        this.onFrame = null;  // Callback when frame is decoded
        this.frameCount = 0;
        this.lastFrameTime = 0;
    }

    // Get codec type
    get type() { throw new Error('Not implemented'); }

    // Get codec name for display
    get name() { throw new Error('Not implemented'); }

    // Initialize the decoder
    init() { throw new Error('Not implemented'); }

    // Cleanup resources
    cleanup() { throw new Error('Not implemented'); }

    // Handle incoming data (from track or datachannel)
    handleData(data) { throw new Error('Not implemented'); }

    // Get stats
    getStats() {
        return {
            frameCount: this.frameCount,
            fps: this.calculateFps()
        };
    }

    calculateFps() {
        const now = performance.now();
        if (this.lastFrameTime === 0) {
            this.lastFrameTime = now;
            return 0;
        }
        const elapsed = now - this.lastFrameTime;
        return elapsed > 0 ? Math.round(1000 / elapsed) : 0;
    }
}

// H.264 decoder using native WebRTC video track
class H264Decoder extends VideoDecoder {
    constructor(videoElement) {
        super(videoElement);
        this.videoElement = videoElement;
    }

    get type() { return CodecType.H264; }
    get name() { return 'H.264 (WebRTC)'; }

    init() {
        logger.info('H264Decoder initialized');
        return true;
    }

    cleanup() {
        if (this.videoElement) {
            this.videoElement.srcObject = null;
        }
    }

    // H.264 frames come through the WebRTC video track, not this method
    // The track is set up by the WebRTC connection directly
    attachTrack(stream) {
        this.videoElement.srcObject = stream;
        this.videoElement.play().catch(e => {
            logger.warn('Video play() failed', { error: e.message });
        });
    }

    handleData(data) {
        // H.264 data is handled by the browser's native WebRTC stack
        // This method is not used for H.264
        logger.warn('H264Decoder.handleData called - this should not happen');
    }
}

// PNG decoder using canvas rendering
// Expects frames with 8-byte timestamp header for latency measurement
class PNGDecoder extends VideoDecoder {
    constructor(canvasElement) {
        super(canvasElement);
        this.canvas = canvasElement;
        this.ctx = null;
        this.pendingBlob = null;

        // Video latency tracking - store totals for averaging
        // Note: We can only measure browser-side latencies (network + decode)
        // because server and browser clocks are not synchronized
        this.decodeLatencyTotal = 0;
        this.latencySamples = 0;
        this.lastLatencyLog = 0;
        this.lastAverageLatency = 0;  // Last calculated average for stats panel

        // Track frame receive times to measure frame intervals
        this.lastFrameReceiveTime = 0;

        // Ping/pong for RTT measurement
        this.pingSequence = 0;
        this.lastReceivedPingSeq = null;  // Track last ping echo we processed
        this.rttTotal = 0;
        this.rttSamples = 0;
        this.lastRttLog = 0;
        this.lastAverageRtt = 0;  // Last calculated average for stats panel
    }

    get type() { return CodecType.PNG; }
    get name() { return 'PNG (DataChannel)'; }

    init() {
        this.ctx = this.canvas.getContext('2d');
        if (!this.ctx) {
            logger.error('Failed to get canvas 2D context');
            return false;
        }
        // Reset latency tracking
        this.decodeLatencyTotal = 0;
        this.latencySamples = 0;
        this.lastFrameReceiveTime = 0;
        logger.info('PNGDecoder initialized');
        return true;
    }

    cleanup() {
        this.ctx = null;
    }

    // Get average video latency in ms
    getAverageLatency() {
        return this.lastAverageLatency;
    }

    getAverageRtt() {
        return this.lastAverageRtt;
    }

    // Get latest ping breakdown (for detailed stats panel)
    getLatestPing() {
        return this.latestPing;
    }

    // Handle PNG data from DataChannel
    // Frame format: [8-byte t1_frame_ready] [4-byte x] [4-byte y] [4-byte width] [4-byte height]
    //               [4-byte frame_width] [4-byte frame_height] [8-byte t4_send_time] [PNG data]
    handleData(data) {
        if (!this.ctx) return;

        const t5_receive = Date.now();  // T5: Browser receive time
        let pngData = data;
        let t1_frame_ready = 0, t4_send = 0;
        let rectX = 0, rectY = 0, rectWidth = 0, rectHeight = 0;
        let frameWidth = 0, frameHeight = 0;

        // Parse metadata header if present (ArrayBuffer with at least 84 bytes + PNG signature)
        if (data instanceof ArrayBuffer && data.byteLength > 92) {
            const view = new DataView(data);

            // Read T1: 8-byte emulator frame ready time (ms since Unix epoch)
            let lo = view.getUint32(0, true);
            let hi = view.getUint32(4, true);
            t1_frame_ready = lo + hi * 0x100000000;

            // Read 4-byte dirty rect coordinates (all little-endian uint32)
            rectX = view.getUint32(8, true);
            rectY = view.getUint32(12, true);
            rectWidth = view.getUint32(16, true);
            rectHeight = view.getUint32(20, true);

            // Read 4-byte full frame resolution
            frameWidth = view.getUint32(24, true);
            frameHeight = view.getUint32(28, true);

            // Read T4: 8-byte server send time (ms since Unix epoch)
            lo = view.getUint32(32, true);
            hi = view.getUint32(36, true);
            t4_send = lo + hi * 0x100000000;

            // Read ping echo with full roundtrip timestamps
            const pingSeq = view.getUint32(40, true);

            // Browser send time (performance.now() milliseconds)
            lo = view.getUint32(44, true);
            hi = view.getUint32(48, true);
            const ping_browser_send_ms = lo + hi * 0x100000000;

            // Server receive time (CLOCK_REALTIME microseconds)
            lo = view.getUint32(52, true);
            hi = view.getUint32(56, true);
            const ping_server_recv_us = lo + hi * 0x100000000;

            // Emulator receive time (CLOCK_REALTIME microseconds)
            lo = view.getUint32(60, true);
            hi = view.getUint32(64, true);
            const ping_emulator_recv_us = lo + hi * 0x100000000;

            // Frame ready time (CLOCK_REALTIME microseconds)
            lo = view.getUint32(68, true);
            hi = view.getUint32(72, true);
            const ping_frame_ready_us = lo + hi * 0x100000000;

            // Server send time (CLOCK_REALTIME microseconds)
            lo = view.getUint32(76, true);
            hi = view.getUint32(80, true);
            const ping_server_send_us = lo + hi * 0x100000000;

            // Calculate all latencies if this frame echoes our ping
            // Handle ping echo if present in this frame
            if (pingSeq > 0) {
                // Check for skipped pings
                if (this.lastReceivedPingSeq > 0 && pingSeq > this.lastReceivedPingSeq + 1) {
                    const skipped = pingSeq - this.lastReceivedPingSeq - 1;
                    logger.warn(`[Ping] WARNING: Skipped ${skipped} ping(s) - jumped from #${this.lastReceivedPingSeq} to #${pingSeq}`);
                }

                // Only process if this is a NEW ping (not a duplicate from multi-frame echo)
                if (pingSeq !== this.lastReceivedPingSeq) {
                    if (debugConfig.debug_perf) {
                        logger.info(`[Ping] New echo #${pingSeq} (browser_send=${ping_browser_send_ms.toFixed(1)}ms)`);
                    }
                    this.lastReceivedPingSeq = pingSeq;

                    // Process ping echo
                    const ping_browser_recv_ms = performance.now();
                    this.handlePingEcho(pingSeq, ping_browser_send_ms, ping_server_recv_us,
                                       ping_emulator_recv_us, ping_frame_ready_us,
                                       ping_server_send_us, ping_browser_recv_ms);
                }
                // Else: duplicate echo (expected due to 5-frame echo), silently ignore
            }
            // Note: Pings are sent every 1 second and echoed in 5 consecutive frames

            // PNG data starts after 84-byte header
            pngData = data.slice(84);
        }

        // Create blob from PNG data
        const blob = pngData instanceof Blob ? pngData : new Blob([pngData], { type: 'image/png' });

        createImageBitmap(blob).then(bitmap => {
            const t6_draw = performance.now();  // Draw complete time (use performance.now for accuracy)

            // Calculate decode latency (receive to draw)
            // We use performance.now() for both timestamps to avoid clock skew
            const t5_receive_perf = performance.now() - (Date.now() - t5_receive);
            const decodeLatency = t6_draw - t5_receive_perf;

            // Track decode latency
            if (decodeLatency >= 0 && decodeLatency < 1000) {
                this.decodeLatencyTotal += decodeLatency;
                this.latencySamples++;
            }

            // Resize canvas based on explicit frame dimensions from server
            // This ensures canvas is the correct size even when receiving dirty rects
            if (frameWidth > 0 && frameHeight > 0) {
                if (this.canvas.width !== frameWidth || this.canvas.height !== frameHeight) {
                    this.canvas.width = frameWidth;
                    this.canvas.height = frameHeight;
                    if (debugConfig.debug_mode_switch) {
                        logger.info('Canvas resized', { width: frameWidth, height: frameHeight });
                    }
                }
            }

            // Draw bitmap at dirty rect position
            // For full frames: rectX=0, rectY=0, bitmap size = canvas size
            // For dirty rects: rectX, rectY specify where to draw the smaller bitmap
            this.ctx.drawImage(bitmap, rectX, rectY);
            this.frameCount++;
            this.lastFrameTime = performance.now();

            if (this.onFrame) {
                this.onFrame(this.frameCount);
            }

            // Log latency stats periodically (every 3 seconds)
            const now = performance.now();
            if (now - this.lastLatencyLog > 3000 && this.latencySamples > 0) {
                const avgDecode = this.decodeLatencyTotal / this.latencySamples;
                this.lastAverageLatency = avgDecode;  // Save for stats panel

                // Log decode latency (brief, on same line as other stats)
                if (debugConfig.debug_perf) {
                    logger.info(`Decode latency: ${avgDecode.toFixed(1)}ms (${this.latencySamples} samples)`);
                }

                // Reset for next interval
                this.decodeLatencyTotal = 0;
                this.latencySamples = 0;
                this.lastLatencyLog = now;
            }
        }).catch(e => {
            logger.error('Failed to decode PNG', { error: e.message });
        });
    }

    handlePingEcho(sequence, browser_send_ms, server_recv_us, emulator_recv_us,
                   frame_ready_us, server_send_us, browser_recv_ms) {
        // Calculate latencies at each hop
        // Note: browser_send/recv use browser clock (performance.now())
        //       server/emulator timestamps use CLOCK_REALTIME (microseconds)

        // Total RTT (browser clock only - accurate!)
        const total_rtt_ms = browser_recv_ms - browser_send_ms;

        // Server-side latencies (same clock - accurate!)
        const ipc_latency_us = emulator_recv_us - server_recv_us;     // Server → Emulator IPC
        const frame_wait_us = frame_ready_us - emulator_recv_us;      // Wait for next frame
        const encode_send_us = server_send_us - frame_ready_us;       // PNG encode + send

        // Convert microseconds to milliseconds
        const ipc_latency_ms = ipc_latency_us / 1000.0;
        const frame_wait_ms = frame_wait_us / 1000.0;
        const encode_send_ms = encode_send_us / 1000.0;

        // Network latency (estimated as remainder after subtracting known server-side latencies)
        const server_side_total_ms = ipc_latency_ms + frame_wait_ms + encode_send_ms;
        const network_ms = total_rtt_ms - server_side_total_ms;

        // Get current time for logging and stats
        const now = performance.now();

        // Track total RTT for averaging (only if reasonable values)
        // Permissive: accept RTT up to 30 seconds (handles slow connections, breakpoints, etc.)
        if (total_rtt_ms > 0 && total_rtt_ms < 30000) {
            this.rttTotal += total_rtt_ms;
            this.rttSamples++;
        } else if (total_rtt_ms >= 30000) {
            // Log unusually high RTT but don't include in average
            logger.warn(`[Ping] Unusually high RTT: ${total_rtt_ms.toFixed(1)}ms (not included in average)`);
        }

        // Store latest ping breakdown for stats panel
        this.latestPing = {
            sequence: sequence,
            total_rtt_ms: total_rtt_ms,
            network_ms: network_ms,
            ipc_ms: ipc_latency_ms,
            wait_ms: frame_wait_ms,
            encode_ms: encode_send_ms,
            timestamp: now
        };

        // Log RTT for EVERY ping (not just every 3 seconds)
        // This helps detect skipped pings and shows real-time latency
        const avgRtt = this.rttSamples > 0 ? this.rttTotal / this.rttSamples : total_rtt_ms;
        if (debugConfig.debug_perf) {
            const rttLog = `Ping #${sequence} RTT ${total_rtt_ms.toFixed(1)}ms: ` +
                          `net=${network_ms.toFixed(1)}ms ipc=${ipc_latency_ms.toFixed(1)}ms ` +
                          `wait=${frame_wait_ms.toFixed(1)}ms enc=${encode_send_ms.toFixed(1)}ms | ` +
                          `avg=${avgRtt.toFixed(1)}ms (${this.rttSamples})`;
            logger.info(rttLog);
        }

        // Reset averaging every 10 pings to keep running average current
        if (this.rttSamples >= 10) {
            this.lastAverageRtt = this.rttTotal / this.rttSamples;  // Save for stats panel
            this.rttTotal = 0;
            this.rttSamples = 0;
        }

        this.lastRttLog = now;
    }

    sendPing(dataChannel) {
        if (!dataChannel || dataChannel.readyState !== 'open') return;

        this.pingSequence++;
        const timestamp = performance.now();
        const msg = `P${this.pingSequence},${timestamp}`;
        dataChannel.send(msg);
    }
}

// Raw RGBA decoder using canvas rendering
class RawDecoder extends VideoDecoder {
    constructor(canvasElement) {
        super(canvasElement);
        this.canvas = canvasElement;
        this.ctx = null;
        this.imageData = null;
        this.expectedWidth = 0;
        this.expectedHeight = 0;
    }

    get type() { return CodecType.RAW; }
    get name() { return 'Raw RGBA (DataChannel)'; }

    init() {
        this.ctx = this.canvas.getContext('2d');
        if (!this.ctx) {
            logger.error('Failed to get canvas 2D context');
            return false;
        }
        logger.info('RawDecoder initialized');
        return true;
    }

    cleanup() {
        this.ctx = null;
        this.imageData = null;
    }

    // Set expected dimensions (from signaling)
    setDimensions(width, height) {
        this.expectedWidth = width;
        this.expectedHeight = height;
        this.canvas.width = width;
        this.canvas.height = height;
        this.imageData = this.ctx.createImageData(width, height);
        logger.info('RawDecoder dimensions set', { width, height });
    }

    // Handle raw RGBA data from DataChannel
    handleData(data) {
        if (!this.ctx || !this.imageData) return;

        // data should be an ArrayBuffer containing RGBA pixels
        const pixels = new Uint8ClampedArray(data);

        // Verify size matches
        const expectedSize = this.expectedWidth * this.expectedHeight * 4;
        if (pixels.length !== expectedSize) {
            logger.warn('Raw frame size mismatch', {
                received: pixels.length,
                expected: expectedSize
            });
            return;
        }

        this.imageData.data.set(pixels);
        this.ctx.putImageData(this.imageData, 0, 0);
        this.frameCount++;
        this.lastFrameTime = performance.now();

        if (this.onFrame) {
            this.onFrame(this.frameCount);
        }
    }
}

// Factory to create the right decoder based on codec type
function createDecoder(codecType, element) {
    switch (codecType) {
        case CodecType.H264:
            return new H264Decoder(element);
        case CodecType.PNG:
            return new PNGDecoder(element);
        case CodecType.RAW:
            return new RawDecoder(element);
        default:
            logger.error('Unknown codec type', { codecType });
            return null;
    }
}


// Main WebRTC Client
class BasiliskWebRTC {
    constructor(videoElement, canvasElement = null) {
        this.video = videoElement;
        this.canvas = canvasElement;
        this.ws = null;
        this.pc = null;
        this.dataChannel = null;
        this.videoTrack = null;
        this.connected = false;
        this.wsUrl = null;

        // Codec/decoder management
        this.codecType = CodecType.H264;  // Default to H.264
        this.decoder = null;

        // Stats tracking
        this.stats = {
            fps: 0,
            bitrate: 0,
            framesDecoded: 0,
            packetsLost: 0,
            jitter: 0,
            codec: 'h264'
        };
        this.lastStatsTime = performance.now();
        this.lastBytesReceived = 0;
        this.lastFramesDecoded = 0;

        // PNG/DataChannel stats
        this.pngStats = {
            framesReceived: 0,
            bytesReceived: 0,
            lastFrameTime: 0,
            avgFrameSize: 0
        };
        this.lastPngFrameCount = 0;
        this.lastPngBytesReceived = 0;

        // Reconnection
        this.reconnectAttempts = 0;
        this.maxReconnectAttempts = 10;
        this.reconnectTimer = null;

        // Frame detection for black screen debugging
        this.firstFrameReceived = false;
        this.frameCheckInterval = null;

        // Ping timer for RTT measurement
        this.pingTimer = null;
    }

    // Set codec type before connecting
    setCodec(codecType) {
        if (this.connected) {
            logger.warn('Cannot change codec while connected');
            return false;
        }
        this.codecType = codecType;
        this.stats.codec = codecType;
        logger.info('Codec set', { codec: codecType });
        return true;
    }

    // Initialize decoder based on codec type
    initDecoder() {
        if (this.decoder) {
            this.decoder.cleanup();
        }

        const element = this.codecType === CodecType.H264 ? this.video : this.canvas;
        if (!element) {
            logger.error('No display element for codec', { codec: this.codecType });
            return false;
        }

        this.decoder = createDecoder(this.codecType, element);
        if (!this.decoder) {
            return false;
        }

        // Show/hide appropriate element
        if (this.video) this.video.style.display = this.codecType === CodecType.H264 ? 'block' : 'none';
        if (this.canvas) this.canvas.style.display = this.codecType !== CodecType.H264 ? 'block' : 'none';

        return this.decoder.init();
    }

    connect(wsUrl) {
        this.wsUrl = wsUrl;
        this.reconnectAttempts = 0;
        this._connect();
    }

    _connect() {
        if (this.ws && this.ws.readyState === WebSocket.OPEN) {
            logger.warn('Already connected');
            return;
        }

        this.cleanup();
        connectionSteps.reset();

        // Initialize decoder for selected codec
        if (!this.initDecoder()) {
            logger.error('Failed to initialize decoder');
            this.updateStatus('Decoder init failed', 'error');
            return;
        }

        if (debugConfig.debug_connection) {
            logger.info('Connecting to signaling server', { url: this.wsUrl, codec: this.codecType });
        }
        this.updateStatus('Connecting...', 'connecting');
        connectionSteps.setActive('ws');

        try {
            this.ws = new WebSocket(this.wsUrl);

            this.ws.onopen = () => this.onWsOpen();
            this.ws.onmessage = (e) => this.onWsMessage(e);
            this.ws.onclose = (e) => this.onWsClose(e);
            this.ws.onerror = (e) => this.onWsError(e);
        } catch (e) {
            logger.error('WebSocket creation failed', { error: e.message });
            this.updateStatus('Connection failed', 'error');
            connectionSteps.setError('ws');
            this.scheduleReconnect();
        }
    }

    onWsOpen() {
        if (debugConfig.debug_connection) {
            logger.info('WebSocket connected');
        }
        connectionSteps.setDone('ws');
        connectionSteps.setActive('offer');
        this.updateStatus('Signaling connected', 'connecting');
        this.updateWebRTCState('ws', 'Open');

        // Request connection (server will tell us which codec to use)
        logger.debug('Sending connect request');
        this.ws.send(JSON.stringify({
            type: 'connect'
        }));
    }

    onWsMessage(event) {
        let msg;
        try {
            msg = JSON.parse(event.data);
        } catch (e) {
            logger.error('Failed to parse message', { data: event.data });
            return;
        }

        logger.debug(`Received: ${msg.type}`, msg.type === 'offer' ? { sdpLength: msg.sdp?.length } : null);

        this.handleSignaling(msg);
    }

    onWsClose(event) {
        logger.warn('WebSocket closed', { code: event.code, reason: event.reason });
        this.updateWebRTCState('ws', 'Closed');
        this.connected = false;
        this.updateStatus('Disconnected', 'error');
        this.scheduleReconnect();
    }

    onWsError(event) {
        logger.error('WebSocket error');
        this.updateWebRTCState('ws', 'Error');
    }

    async handleSignaling(msg) {
        switch (msg.type) {
            case 'welcome':
                if (debugConfig.debug_connection) {
                    logger.info('Server acknowledged connection');
                }
                this.updateOverlayStatus('Waiting for video offer...');
                break;

            case 'connected':
                // Server tells us which codec to use
                if (msg.codec) {
                    const serverCodec = msg.codec === 'h264' ? CodecType.H264 :
                                       msg.codec === 'png' ? CodecType.PNG :
                                       msg.codec === 'raw' ? CodecType.RAW : CodecType.PNG;
                    if (serverCodec !== this.codecType) {
                        if (debugConfig.debug_connection) {
                            logger.info('Server codec', { codec: msg.codec });
                        }
                        this.codecType = serverCodec;
                        // Reinitialize decoder for new codec
                        this.initDecoder();
                    }
                }
                if (debugConfig.debug_connection) {
                    logger.info('Server acknowledged connection', { codec: msg.codec, peer_id: msg.peer_id });
                }
                this.updateOverlayStatus('Waiting for video offer...');
                break;

            case 'offer':
                if (debugConfig.debug_connection) {
                    logger.info('Received SDP offer', { sdpLength: msg.sdp.length });
                }
                connectionSteps.setDone('offer');
                connectionSteps.setActive('ice');
                this.updateOverlayStatus('Processing offer...');

                // Show SDP info in debug panel
                this.updateSdpInfo(msg.sdp);

                await this.handleOffer(msg.sdp);
                break;

            case 'candidate':
                logger.debug('Received ICE candidate', { mid: msg.mid });
                if (this.pc) {
                    try {
                        await this.pc.addIceCandidate(new RTCIceCandidate({
                            candidate: msg.candidate,
                            sdpMid: msg.mid
                        }));
                    } catch (e) {
                        logger.warn('Failed to add ICE candidate', { error: e.message });
                    }
                }
                break;

            case 'error':
                logger.error('Server error', { message: msg.message });
                this.updateStatus('Server error', 'error');
                break;

            default:
                logger.debug('Unknown message type', { type: msg.type });
        }
    }

    async handleOffer(sdp) {
        this.createPeerConnection();

        try {
            const offer = new RTCSessionDescription({ type: 'offer', sdp: sdp });
            await this.pc.setRemoteDescription(offer);
            if (debugConfig.debug_connection) {
                logger.info('Set remote description (offer)');
            }

            const answer = await this.pc.createAnswer();
            await this.pc.setLocalDescription(answer);
            if (debugConfig.debug_connection) {
                logger.info('Created and set local description (answer)');
            }

            // Wait for ICE gathering to complete before sending answer
            // This ensures all candidates are included in the SDP
            await this.waitForIceGathering();

            // Send the final answer with all ICE candidates included
            const finalAnswer = this.pc.localDescription;

            // Debug: check SDP has ICE credentials
            if (!finalAnswer.sdp.includes('a=ice-ufrag:')) {
                logger.error('Answer SDP missing ice-ufrag!', { sdp: finalAnswer.sdp });
            } else if (debugConfig.debug_connection) {
                logger.info('Answer SDP has ICE credentials');
            }

            this.ws.send(JSON.stringify({
                type: 'answer',
                sdp: finalAnswer.sdp
            }));
            logger.debug('Sent SDP answer with ICE candidates');

        } catch (e) {
            logger.error('Failed to handle offer', { error: e.message });
            connectionSteps.setError('offer');
            this.updateStatus('Offer handling failed', 'error');
        }
    }

    waitForIceGathering() {
        return new Promise((resolve) => {
            if (this.pc.iceGatheringState === 'complete') {
                resolve();
                return;
            }

            const checkState = () => {
                if (this.pc.iceGatheringState === 'complete') {
                    this.pc.removeEventListener('icegatheringstatechange', checkState);
                    logger.info('ICE gathering complete, sending answer');
                    resolve();
                }
            };

            this.pc.addEventListener('icegatheringstatechange', checkState);

            // Timeout after 5 seconds - send what we have
            setTimeout(() => {
                this.pc.removeEventListener('icegatheringstatechange', checkState);
                if (debugConfig.debug_connection) {
                    logger.warn('ICE gathering timeout, sending answer with available candidates');
                }
                resolve();
            }, 5000);
        });
    }

    createPeerConnection() {
        if (debugConfig.debug_connection) {
            logger.info('Creating RTCPeerConnection');
        }

        const config = {
            iceServers: [
                { urls: 'stun:stun.l.google.com:19302' },
                { urls: 'stun:stun1.l.google.com:19302' }
            ]
        };

        this.pc = new RTCPeerConnection(config);

        this.pc.ontrack = (e) => this.onTrack(e);
        this.pc.ondatachannel = (e) => this.onDataChannel(e);
        this.pc.onicecandidate = (e) => this.onIceCandidate(e);
        this.pc.oniceconnectionstatechange = () => this.onIceConnectionStateChange();
        this.pc.onicegatheringstatechange = () => this.onIceGatheringStateChange();
        this.pc.onconnectionstatechange = () => this.onConnectionStateChange();
        this.pc.onsignalingstatechange = () => this.onSignalingStateChange();

        this.updateWebRTCState('pc', 'Created');
    }

    onTrack(event) {
        if (debugConfig.debug_connection) {
            logger.info('Track received', { kind: event.track.kind, id: event.track.id });
        }
        connectionSteps.setDone('track');
        connectionSteps.setActive('frames');
        this.updateOverlayStatus('Receiving video stream...');

        if (event.track.kind === 'video') {
            this.videoTrack = event.track;

            // Track state monitoring
            event.track.onmute = () => {
                logger.warn('Video track muted');
                this.updateWebRTCState('track-muted', 'Yes');
            };
            event.track.onunmute = () => {
                logger.info('Video track unmuted');
                this.updateWebRTCState('track-muted', 'No');
            };
            event.track.onended = () => {
                logger.warn('Video track ended');
                this.updateWebRTCState('track-state', 'Ended');
            };

            this.updateWebRTCState('track-state', event.track.readyState);
            this.updateWebRTCState('track-enabled', event.track.enabled ? 'Yes' : 'No');
            this.updateWebRTCState('track-muted', event.track.muted ? 'Yes' : 'No');

            if (event.streams && event.streams[0]) {
                if (debugConfig.debug_connection) {
                    logger.info('Attaching stream to video element', {
                        streamId: event.streams[0].id,
                        trackCount: event.streams[0].getTracks().length
                    });
                }
                this.video.srcObject = event.streams[0];

                // Log all video element events for debugging
                this.video.onloadstart = () => logger.debug('Video: loadstart');
                this.video.onprogress = () => logger.debug('Video: progress');
                this.video.onsuspend = () => logger.debug('Video: suspend');
                this.video.onemptied = () => logger.debug('Video: emptied');
                this.video.oncanplay = () => logger.info('Video: canplay');
                this.video.oncanplaythrough = () => logger.info('Video: canplaythrough');
                this.video.onerror = (e) => logger.error('Video element error', {
                    code: this.video.error?.code,
                    message: this.video.error?.message
                });

                this.video.onloadedmetadata = () => {
                    logger.info('Video metadata loaded', {
                        width: this.video.videoWidth,
                        height: this.video.videoHeight
                    });
                    this.updateWebRTCState('video-size', `${this.video.videoWidth} x ${this.video.videoHeight}`);
                };

                this.video.onloadeddata = () => {
                    logger.info('Video: loadeddata (first frame decoded)', {
                        width: this.video.videoWidth,
                        height: this.video.videoHeight,
                        readyState: this.video.readyState
                    });
                };

                this.video.onplaying = () => {
                    logger.info('Video playing');
                    this.onVideoPlaying();
                };

                this.video.onwaiting = () => {
                    logger.warn('Video waiting/buffering');
                };

                this.video.onstalled = () => {
                    logger.warn('Video stalled');
                };

                this.video.play().catch(e => {
                    logger.warn('Video play() failed', { error: e.message });
                });

                // Log video element state periodically
                setTimeout(() => {
                    logger.debug('Video element state after 2s', {
                        readyState: this.video.readyState,
                        networkState: this.video.networkState,
                        paused: this.video.paused,
                        ended: this.video.ended,
                        videoWidth: this.video.videoWidth,
                        videoHeight: this.video.videoHeight,
                        currentTime: this.video.currentTime,
                        srcObject: this.video.srcObject ? 'set' : 'null'
                    });
                }, 2000);

                // Start frame detection
                this.startFrameDetection();
            } else {
                logger.warn('No stream in track event, creating MediaStream manually');
                const stream = new MediaStream([event.track]);
                this.video.srcObject = stream;
                this.video.play().catch(e => {
                    logger.warn('Video play() failed', { error: e.message });
                });
            }
        }
    }

    startFrameDetection() {
        // Check if we're actually receiving frames
        this.frameCheckInterval = setInterval(() => {
            if (this.video.videoWidth > 0 && this.video.videoHeight > 0) {
                if (!this.firstFrameReceived) {
                    this.firstFrameReceived = true;
                    connectionSteps.setDone('frames');
                    if (debugConfig.debug_connection) {
                        logger.info('First frame received!', {
                            width: this.video.videoWidth,
                            height: this.video.videoHeight
                        });
                    }

                    // Check if video appears black
                    this.checkForBlackScreen();
                }
            }
        }, 100);
    }

    checkForBlackScreen() {
        // Create a canvas to sample pixels
        const canvas = document.createElement('canvas');
        const ctx = canvas.getContext('2d');
        canvas.width = 10;
        canvas.height = 10;

        setTimeout(() => {
            try {
                ctx.drawImage(this.video, 0, 0, 10, 10);
                const imageData = ctx.getImageData(0, 0, 10, 10);
                const data = imageData.data;

                let totalBrightness = 0;
                for (let i = 0; i < data.length; i += 4) {
                    totalBrightness += (data[i] + data[i + 1] + data[i + 2]) / 3;
                }
                const avgBrightness = totalBrightness / (data.length / 4);

                if (avgBrightness < 5) {
                    logger.warn('VIDEO APPEARS BLACK - Average brightness: ' + avgBrightness.toFixed(1));
                    logger.warn('Possible causes: encoder issue, stride mismatch, no frames from emulator');
                } else {
                    logger.info('Video brightness check passed', { avgBrightness: avgBrightness.toFixed(1) });
                }
            } catch (e) {
                logger.debug('Could not sample video pixels', { error: e.message });
            }
        }, 1000);
    }

    onVideoPlaying() {
        this.connected = true;
        this.updateStatus('Connected', 'connected');
        this.hideOverlay();
        this.updateConnectionUI(true);
        logger.info('Stream is playing');
    }

    onDataChannel(event) {
        if (debugConfig.debug_connection) {
            logger.info('Data channel received', { label: event.channel.label });
        }
        this.dataChannel = event.channel;
        this.setupDataChannel();
    }

    onIceCandidate(event) {
        // We now wait for ICE gathering complete and send all candidates in the answer SDP
        // So we don't need to send individual candidates via trickle ICE
        if (event.candidate) {
            logger.debug('ICE candidate gathered', { candidate: event.candidate.candidate.substring(0, 50) + '...' });
        } else {
            logger.debug('ICE gathering complete (null candidate)');
        }
    }

    onIceConnectionStateChange() {
        const state = this.pc.iceConnectionState;
        if (debugConfig.debug_connection) {
            logger.info('ICE connection state', { state });
        }
        this.updateWebRTCState('ice', state);

        if (state === 'connected' || state === 'completed') {
            connectionSteps.setDone('ice');
        } else if (state === 'failed') {
            connectionSteps.setError('ice');
            this.updateStatus('ICE connection failed', 'error');
            logger.error('ICE connection failed - may need TURN server');
        } else if (state === 'disconnected') {
            logger.warn('ICE disconnected - may recover');
        }
    }

    onIceGatheringStateChange() {
        const state = this.pc.iceGatheringState;
        logger.debug('ICE gathering state', { state });
        this.updateWebRTCState('ice-gathering', state);
    }

    onConnectionStateChange() {
        const state = this.pc.connectionState;
        if (debugConfig.debug_connection) {
            logger.info('Connection state', { state });
        }
        this.updateWebRTCState('pc', state);

        if (state === 'failed') {
            this.updateStatus('Connection failed', 'error');
            this.connected = false;
        } else if (state === 'disconnected') {
            this.updateStatus('Disconnected', 'error');
            this.connected = false;
        }
    }

    onSignalingStateChange() {
        const state = this.pc.signalingState;
        logger.debug('Signaling state', { state });
        this.updateWebRTCState('signaling', state);
    }

    setupDataChannel() {
        if (!this.dataChannel) return;

        // Set binary type for receiving PNG/RAW frames
        this.dataChannel.binaryType = 'arraybuffer';

        this.dataChannel.onopen = () => {
            if (debugConfig.debug_connection) {
                logger.info('Data channel open');
            }
            this.updateWebRTCState('dc', 'Open');
            this.setupInputHandlers();
            this.startPingTimer();
        };

        this.dataChannel.onclose = () => {
            logger.warn('Data channel closed');
            this.updateWebRTCState('dc', 'Closed');
        };

        this.dataChannel.onerror = (e) => {
            logger.error('Data channel error');
            this.updateWebRTCState('dc', 'Error');
        };

        // Handle incoming messages (frames for PNG/RAW, or other messages)
        this.dataChannel.onmessage = (event) => {
            if (event.data instanceof ArrayBuffer) {
                // Binary data - this is a video frame for PNG/RAW codec
                if (this.decoder && this.codecType !== CodecType.H264) {
                    this.decoder.handleData(event.data);

                    // Track PNG/RAW frame stats
                    this.pngStats.framesReceived++;
                    this.pngStats.bytesReceived += event.data.byteLength;
                    this.pngStats.lastFrameTime = performance.now();
                    this.pngStats.avgFrameSize = this.pngStats.bytesReceived / this.pngStats.framesReceived;

                    // Update first frame received flag
                    if (!this.firstFrameReceived) {
                        this.firstFrameReceived = true;
                        connectionSteps.setDone('frames');
                        if (debugConfig.debug_connection) {
                            logger.info('First frame received via DataChannel');
                        }

                        // For PNG/RAW codecs, mark as connected and hide overlay
                        this.connected = true;
                        this.updateStatus('Connected', 'connected');
                        this.hideOverlay();
                        this.updateConnectionUI(true);
                    }
                }
            } else {
                // Text data - might be control messages
                logger.debug('DataChannel text message', { data: event.data });
            }
        };
    }

    setupInputHandlers() {
        // Use the appropriate display element (video for H.264, canvas for PNG/RAW)
        const displayElement = this.codecType === CodecType.H264 ? this.video : this.canvas;
        if (!displayElement) return;

        // Click to capture mouse (pointer lock for relative movement)
        displayElement.addEventListener('click', () => {
            if (!document.pointerLockElement) {
                displayElement.requestPointerLock();
            }
        });

        // Mouse move - only when pointer is locked (relative movement)
        // Include timestamp for end-to-end latency measurement
        document.addEventListener('mousemove', (e) => {
            if (document.pointerLockElement === displayElement) {
                const ts = performance.now();
                this.sendRaw('M' + e.movementX + ',' + e.movementY + ',' + ts.toFixed(1));
            }
        });

        // Mouse buttons (also on document when pointer locked)
        const handleMouseDown = (e) => {
            if (document.pointerLockElement === displayElement) {
                e.preventDefault();
                const ts = performance.now();
                this.sendRaw('D' + e.button + ',' + ts.toFixed(1));
            }
        };
        const handleMouseUp = (e) => {
            if (document.pointerLockElement === displayElement) {
                e.preventDefault();
                const ts = performance.now();
                this.sendRaw('U' + e.button + ',' + ts.toFixed(1));
            }
        };
        document.addEventListener('mousedown', handleMouseDown);
        document.addEventListener('mouseup', handleMouseUp);

        displayElement.addEventListener('contextmenu', (e) => e.preventDefault());

        // Keyboard
        document.addEventListener('keydown', (e) => {
            if (!this.connected) return;
            if (e.target.tagName === 'INPUT' || e.target.tagName === 'TEXTAREA') return;
            e.preventDefault();
            this.sendRaw('K' + e.keyCode);
        });

        document.addEventListener('keyup', (e) => {
            if (!this.connected) return;
            if (e.target.tagName === 'INPUT' || e.target.tagName === 'TEXTAREA') return;
            e.preventDefault();
            this.sendRaw('k' + e.keyCode);
        });

        if (debugConfig.debug_connection) {
            logger.info('Input handlers registered (pointer lock mode)', { element: displayElement.tagName });
        }
    }

    // Send raw text message (simple protocol: M dx,dy | D btn | U btn | K code | k code)
    sendRaw(msg) {
        if (this.dataChannel && this.dataChannel.readyState === 'open') {
            this.dataChannel.send(msg);
        }
    }

    // Legacy JSON method (kept for restart/shutdown commands)
    sendInput(msg) {
        if (this.dataChannel && this.dataChannel.readyState === 'open') {
            this.dataChannel.send(JSON.stringify(msg));
        }
    }

    startPingTimer() {
        // Stop any existing ping timer
        if (this.pingTimer) {
            clearInterval(this.pingTimer);
        }

        // Send ping every 1 second for RTT measurement
        // Only when using PNG/RAW codec (decoder has sendPing method)
        this.pingTimer = setInterval(() => {
            if (this.decoder && this.decoder.sendPing && this.dataChannel) {
                this.decoder.sendPing(this.dataChannel);
            }
        }, 1000);
    }

    scheduleReconnect() {
        if (this.reconnectTimer) {
            clearTimeout(this.reconnectTimer);
        }

        if (this.reconnectAttempts >= this.maxReconnectAttempts) {
            logger.error('Max reconnection attempts reached');
            this.updateStatus('Connection failed - click Connect to retry', 'error');
            return;
        }

        this.reconnectAttempts++;
        const delay = Math.min(1000 * Math.pow(2, this.reconnectAttempts - 1), 30000);
        logger.info(`Reconnecting in ${delay / 1000}s (attempt ${this.reconnectAttempts}/${this.maxReconnectAttempts})`);
        this.updateOverlayStatus(`Reconnecting in ${Math.round(delay / 1000)}s...`);

        this.reconnectTimer = setTimeout(() => {
            if (!this.connected) {
                this._connect();
            }
        }, delay);
    }

    cleanup() {
        if (this.frameCheckInterval) {
            clearInterval(this.frameCheckInterval);
            this.frameCheckInterval = null;
        }
        if (this.reconnectTimer) {
            clearTimeout(this.reconnectTimer);
            this.reconnectTimer = null;
        }
        if (this.pingTimer) {
            clearInterval(this.pingTimer);
            this.pingTimer = null;
        }
        if (this.decoder) {
            this.decoder.cleanup();
            this.decoder = null;
        }
        if (this.dataChannel) {
            this.dataChannel.close();
            this.dataChannel = null;
        }
        if (this.pc) {
            this.pc.close();
            this.pc = null;
        }
        this.videoTrack = null;
        this.firstFrameReceived = false;
    }

    disconnect() {
        logger.info('Disconnecting');
        this.reconnectAttempts = this.maxReconnectAttempts; // Prevent auto-reconnect
        this.cleanup();
        if (this.ws) {
            this.ws.close();
            this.ws = null;
        }
        this.connected = false;
        this.updateStatus('Disconnected', 'error');
        this.updateConnectionUI(false);
        this.showOverlay('Disconnected', 'Click Connect to reconnect');
    }

    // Stats collection
    async updateStats() {
        if (!this.connected) return;

        const now = performance.now();
        const elapsed = (now - this.lastStatsTime) / 1000;

        // For PNG/RAW codecs, calculate stats from our own tracking
        if (this.codecType !== CodecType.H264) {
            if (elapsed > 0) {
                const framesDelta = this.pngStats.framesReceived - this.lastPngFrameCount;
                const bytesDelta = this.pngStats.bytesReceived - this.lastPngBytesReceived;

                this.stats.fps = Math.round(framesDelta / elapsed);
                this.stats.bitrate = Math.round((bytesDelta * 8 / elapsed) / 1000);
                this.stats.framesDecoded = this.pngStats.framesReceived;
                this.stats.packetsLost = 0;  // DataChannel is reliable
                this.stats.jitter = 0;
                this.stats.codec = this.codecType;

                // Log detailed stats every 3 seconds
                if (debugConfig.debug_perf && (!this.lastDetailedStatsTime || (now - this.lastDetailedStatsTime) > 3000)) {
                    const avgFrameKB = Math.round(this.pngStats.avgFrameSize / 1024);
                    const totalMB = (this.pngStats.bytesReceived / (1024 * 1024)).toFixed(1);
                    logger.info(`fps=${this.stats.fps} | video: frames=${this.pngStats.framesReceived} recv=${totalMB}MB avg=${avgFrameKB}KB | bitrate: ${this.stats.bitrate}kbps`);
                    this.lastDetailedStatsTime = now;
                }

                this.lastPngFrameCount = this.pngStats.framesReceived;
                this.lastPngBytesReceived = this.pngStats.bytesReceived;
            }

            this.lastStatsTime = now;
            this.updateStatsDisplay();
            return;
        }

        // For H.264, use WebRTC stats
        if (!this.pc) return;

        try {
            const stats = await this.pc.getStats();

            stats.forEach(report => {
                if (report.type === 'inbound-rtp' && report.kind === 'video') {
                    const bytesReceived = report.bytesReceived || 0;
                    const framesDecoded = report.framesDecoded || 0;
                    const packetsLost = report.packetsLost || 0;
                    const packetsReceived = report.packetsReceived || 0;
                    const framesDropped = report.framesDropped || 0;
                    const framesReceived = report.framesReceived || 0;
                    const keyFramesDecoded = report.keyFramesDecoded || 0;
                    const totalDecodeTime = report.totalDecodeTime || 0;
                    const jitter = report.jitter || 0;

                    if (elapsed > 0) {
                        this.stats.fps = Math.round((framesDecoded - this.lastFramesDecoded) / elapsed);
                        const bps = (bytesReceived - this.lastBytesReceived) * 8 / elapsed;
                        this.stats.bitrate = Math.round(bps / 1000);
                    }

                    this.stats.framesDecoded = framesDecoded;
                    this.stats.packetsLost = packetsLost;
                    this.stats.packetsReceived = packetsReceived;
                    this.stats.framesDropped = framesDropped;
                    this.stats.framesReceived = framesReceived;
                    this.stats.keyFramesDecoded = keyFramesDecoded;
                    this.stats.jitter = Math.round(jitter * 1000);

                    // Log detailed stats every 3 seconds
                    if (!this.lastDetailedStatsTime || (now - this.lastDetailedStatsTime) > 3000) {
                        logger.info('RTP stats', {
                            packetsRecv: packetsReceived,
                            packetsLost: packetsLost,
                            bytesRecv: bytesReceived,
                            framesRecv: framesReceived,
                            framesDecoded: framesDecoded,
                            framesDropped: framesDropped,
                            keyFrames: keyFramesDecoded,
                            decodeTime: totalDecodeTime.toFixed(2) + 's'
                        });
                        this.lastDetailedStatsTime = now;
                    }

                    this.lastBytesReceived = bytesReceived;
                    this.lastFramesDecoded = framesDecoded;
                }
            });

            this.lastStatsTime = now;
            this.updateStatsDisplay();

        } catch (e) {
            logger.debug('Stats error', { error: e.message });
        }
    }

    updateStatsDisplay() {
        // Header stats
        const fpsEl = document.getElementById('fps-display');
        const bitrateEl = document.getElementById('bitrate-display');
        if (fpsEl) fpsEl.textContent = `FPS: ${this.stats.fps}`;
        if (bitrateEl) bitrateEl.textContent = `${this.stats.bitrate} kbps`;

        // Get resolution from appropriate element
        let width = 0, height = 0;
        if (this.codecType === CodecType.H264 && this.video) {
            width = this.video.videoWidth;
            height = this.video.videoHeight;
        } else if (this.canvas) {
            width = this.canvas.width;
            height = this.canvas.height;
        }

        // Footer resolution
        const resEl = document.getElementById('resolution');
        if (resEl && width) {
            const codecLabel = this.codecType === CodecType.H264 ? 'H.264' :
                              this.codecType === CodecType.PNG ? 'PNG' : 'RAW';
            resEl.textContent = `${width} x ${height} (${codecLabel})`;
        }

        // Debug panel stats
        const statFps = document.getElementById('stat-fps');
        const statBitrate = document.getElementById('stat-bitrate');
        const statRes = document.getElementById('stat-resolution');
        const statFrames = document.getElementById('stat-frames');
        const statLost = document.getElementById('stat-packets-lost');
        const statJitter = document.getElementById('stat-jitter');

        if (statFps) {
            statFps.textContent = this.stats.fps;
            statFps.className = 'value ' + (this.stats.fps >= 25 ? 'good' : this.stats.fps >= 15 ? 'warn' : 'bad');
        }
        if (statBitrate) statBitrate.textContent = `${this.stats.bitrate} kbps`;
        if (statRes && width) {
            statRes.textContent = `${width} x ${height}`;
        }
        if (statFrames) statFrames.textContent = this.stats.framesDecoded.toLocaleString();
        if (statLost) {
            statLost.textContent = this.stats.packetsLost;
            statLost.className = 'value ' + (this.stats.packetsLost === 0 ? 'good' : 'bad');
        }
        if (statJitter) statJitter.textContent = `${this.stats.jitter} ms`;
    }

    // UI helpers
    updateStatus(text, type = '') {
        const statusEl = document.getElementById('connection-status');
        const dotEl = document.getElementById('status-dot');

        if (statusEl) statusEl.textContent = text;
        if (dotEl) {
            dotEl.className = 'status-dot';
            if (type) dotEl.classList.add(type);
        }
    }

    updateOverlayStatus(text) {
        const el = document.getElementById('overlay-status');
        if (el) el.textContent = text;
    }

    showOverlay(title, status) {
        const overlay = document.getElementById('overlay');
        const titleEl = document.getElementById('overlay-title');
        const statusEl = document.getElementById('overlay-status');

        if (overlay) overlay.classList.remove('hidden');
        if (titleEl) titleEl.textContent = title || 'Connecting to Basilisk II';
        if (statusEl) statusEl.textContent = status || 'Initializing...';
    }

    hideOverlay() {
        const overlay = document.getElementById('overlay');
        if (overlay) overlay.classList.add('hidden');
    }

    updateConnectionUI(connected) {
        const btn = document.getElementById('connect-btn');
        if (btn) {
            btn.textContent = connected ? 'Disconnect' : 'Connect';
            btn.classList.toggle('primary', !connected);
        }
    }

    updateWebRTCState(key, value) {
        const stateMap = {
            'ws': 'ws-state',
            'pc': 'pc-state',
            'ice': 'ice-state',
            'ice-gathering': 'ice-gathering-state',
            'signaling': 'signaling-state',
            'dc': 'dc-state',
            'track-state': 'track-state',
            'track-enabled': 'track-enabled',
            'track-muted': 'track-muted',
            'video-size': 'video-size'
        };

        const elId = stateMap[key];
        if (!elId) return;

        const el = document.getElementById(elId);
        if (!el) return;

        el.textContent = value;

        // Color coding
        el.className = 'value';
        const goodStates = ['connected', 'complete', 'completed', 'stable', 'Open', 'open', 'Yes', 'live'];
        const badStates = ['failed', 'closed', 'Closed', 'Error', 'disconnected', 'ended'];
        const connectingStates = ['connecting', 'checking', 'new', 'gathering'];

        const lowerValue = value.toLowerCase();
        if (goodStates.some(s => lowerValue.includes(s.toLowerCase()))) {
            el.classList.add('good');
        } else if (badStates.some(s => lowerValue.includes(s.toLowerCase()))) {
            el.classList.add('bad');
        } else if (connectingStates.some(s => lowerValue.includes(s.toLowerCase()))) {
            el.classList.add('connecting');
        }
    }

    updateSdpInfo(sdp) {
        const el = document.getElementById('sdp-info');
        if (!el) return;

        // Extract key info from SDP
        const lines = sdp.split('\n');
        const info = [];

        lines.forEach(line => {
            if (line.startsWith('m=video')) info.push(line);
            if (line.startsWith('a=rtpmap')) info.push(line);
            if (line.startsWith('a=fmtp')) info.push(line.substring(0, 80) + (line.length > 80 ? '...' : ''));
        });

        el.textContent = info.join('\n') || 'No video media found in SDP';
    }
}

// Global client instance
let client = null;
let statsInterval = null;

// Get base path from current page location (for reverse proxy support)
// e.g., /macemu/ from /macemu/index.html, or empty string for root
function getBasePath() {
    const pathParts = window.location.pathname.split('/');
    pathParts.pop(); // Remove filename
    const basePath = pathParts.join('/');
    return basePath ? basePath + '/' : '';
}

// Build API URL relative to current page location
function getApiUrl(endpoint) {
    return `${getBasePath()}api/${endpoint}`;
}

// Build WebSocket URL for signaling server
// Can be overridden via:
//   - URL param: ?ws=wss://example.com/path
//   - <meta name="ws-url" content="wss://example.com/path">
// Default: ws://hostname:8090 (signaling server port)
function getWebSocketUrl() {
    // Check URL parameter first
    const urlParams = new URLSearchParams(window.location.search);
    const wsParam = urlParams.get('ws');
    if (wsParam) return wsParam;

    // Check meta tag
    const wsMeta = document.querySelector('meta[name="ws-url"]');
    if (wsMeta?.content) return wsMeta.content;

    // Default: use port 8090 for signaling (separate from HTTP server on 8000)
    const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
    const hostname = window.location.hostname;
    return `${protocol}//${hostname}:8090/`;
}

// Log browser and codec capabilities
async function logBrowserCapabilities() {
    // Browser info
    const ua = navigator.userAgent;
    const browserMatch = ua.match(/(Firefox|Chrome|Safari|Edge|OPR)\/(\d+)/);
    const browser = browserMatch ? `${browserMatch[1]} ${browserMatch[2]}` : 'Unknown';

    logger.info('Browser', {
        name: browser,
        userAgent: ua.substring(0, 100) + (ua.length > 100 ? '...' : '')
    });

    // Check H.264 decode support via MediaCapabilities API
    if ('mediaCapabilities' in navigator) {
        try {
            // Test H.264 Constrained Baseline Level 5.1 (what server sends)
            const h264Config = {
                type: 'file',
                video: {
                    contentType: 'video/mp4; codecs="avc1.42e033"',  // CBP Level 5.1
                    width: 1920,
                    height: 1080,
                    framerate: 30,
                    bitrate: 40000000  // 40 Mbps
                }
            };
            const h264Result = await navigator.mediaCapabilities.decodingInfo(h264Config);
            if (debugConfig.debug_connection) {
                logger.info('H.264 CBP L5.1 decode', {
                    supported: h264Result.supported,
                    smooth: h264Result.smooth,
                    powerEfficient: h264Result.powerEfficient
                });
            }

            // Also test Level 3.1 for comparison
            const h264L31Config = {
                type: 'file',
                video: {
                    contentType: 'video/mp4; codecs="avc1.42e01f"',  // CBP Level 3.1
                    width: 1280,
                    height: 720,
                    framerate: 30,
                    bitrate: 10000000
                }
            };
            const h264L31Result = await navigator.mediaCapabilities.decodingInfo(h264L31Config);
            if (debugConfig.debug_connection) {
                logger.info('H.264 CBP L3.1 decode', {
                    supported: h264L31Result.supported,
                    smooth: h264L31Result.smooth,
                    powerEfficient: h264L31Result.powerEfficient
                });
            }
        } catch (e) {
            logger.warn('MediaCapabilities check failed', { error: e.message });
        }
    } else {
        logger.warn('MediaCapabilities API not available');
    }

    // Check RTCRtpReceiver capabilities
    if (RTCRtpReceiver.getCapabilities) {
        const videoCaps = RTCRtpReceiver.getCapabilities('video');
        if (videoCaps) {
            const h264Codecs = videoCaps.codecs.filter(c => c.mimeType.includes('H264') || c.mimeType.includes('h264'));
            if (debugConfig.debug_connection) {
                logger.info('WebRTC H.264 codecs', {
                    count: h264Codecs.length,
                    profiles: h264Codecs.map(c => c.sdpFmtpLine || 'default').join('; ')
                });
            }
        }
    }

    // Hardware acceleration check (Chrome-specific)
    if ('gpu' in navigator) {
        try {
            const adapter = await navigator.gpu?.requestAdapter();
            if (adapter) {
                logger.info('WebGPU available', { name: adapter.name || 'unknown' });
            }
        } catch (e) {
            // WebGPU not available
        }
    }
}

function initClient() {
    logger.init();
    logger.info('Basilisk II WebRTC Client initialized');

    // Log browser capabilities asynchronously
    logBrowserCapabilities().catch(e => logger.warn('Capability check failed', { error: e.message }));

    const video = document.getElementById('display');
    const canvas = document.getElementById('display-canvas');
    if (!video) {
        logger.error('No video element found');
        return;
    }

    client = new BasiliskWebRTC(video, canvas);

    // Codec is now determined by server (from prefs file webcodec setting)
    // Client will receive codec in "connected" message and switch if needed

    // Start stats collection
    statsInterval = setInterval(() => {
        if (client) client.updateStats();
    }, 1000);

    // Auto-connect
    const wsUrl = getWebSocketUrl();
    logger.info('Auto-connecting', { url: wsUrl, codec: client.codecType });
    client.connect(wsUrl);
}

function toggleConnection() {
    if (!client) {
        initClient();
        return;
    }

    if (client.connected) {
        client.disconnect();
    } else {
        client.reconnectAttempts = 0;
        const wsUrl = getWebSocketUrl();
        client.connect(wsUrl);
    }
}

function toggleFullscreen() {
    const container = document.getElementById('display-container') || document.body;

    if (document.fullscreenElement) {
        document.exitFullscreen();
    } else {
        container.requestFullscreen().catch(e => {
            logger.warn('Fullscreen failed', { error: e.message });
        });
    }
}

function toggleDebugPanel() {
    const panel = document.getElementById('debug-panel');
    const btn = document.getElementById('debug-toggle');

    if (panel) {
        panel.classList.toggle('collapsed');
        if (btn) btn.classList.toggle('active', !panel.classList.contains('collapsed'));
    }
}

function showDebugTab(tabName) {
    // Update tab buttons
    document.querySelectorAll('.debug-tab').forEach(tab => {
        tab.classList.toggle('active', tab.textContent.toLowerCase() === tabName);
    });

    // Update panes
    document.querySelectorAll('.debug-pane').forEach(pane => {
        pane.classList.toggle('active', pane.id === `${tabName}-pane`);
    });
}

function clearLog() {
    logger.clear();
}

// Known ROM database with checksums and recommendations
const ROM_DATABASE = {
    // Mac II family
    '97851db6': { name: 'Mac II', model: 5, recommended: false },
    'b2e362a8': { name: 'Mac IIx', model: 6, recommended: false },
    '4147dd77': { name: 'Mac IIcx', model: 7, recommended: false },
    '368cadfe': { name: 'Mac IIci', model: 11, recommended: true },
    '36b7fb6c': { name: 'Mac IIfx', model: 13, recommended: false },

    // Quadra family
    '420dbff3': { name: 'Quadra 700', model: 22, recommended: true },
    '3dc27823': { name: 'Quadra 900', model: 14, recommended: true },
    '49579803': { name: 'Quadra 950', model: 26, recommended: false },

    // LC/Performa family
    '350eacf0': { name: 'LC', model: 19, recommended: false },
    '35c28f5f': { name: 'LC II', model: 37, recommended: false },
    'ecbbc41c': { name: 'LC III', model: 27, recommended: false },
    'ecd99dc0': { name: 'LC 475/Performa 475', model: 44, recommended: false },
    'ff7439ee': { name: 'LC 575/Performa 575', model: 60, recommended: false },

    // Classic family
    'a49f9914': { name: 'Classic', model: 17, recommended: false },
    '3193670e': { name: 'Classic II', model: 23, recommended: false },

    // Common alternate checksums
    '9779d2c4': { name: 'Mac IIci (alternate)', model: 11, recommended: false },
    'e33b2724': { name: 'Quadra 610', model: 52, recommended: false },
    'f1a6f343': { name: 'Quadra 650', model: 36, recommended: false },
    'f1acad13': { name: 'Quadra 800', model: 35, recommended: false },
};

function getRomInfo(checksum) {
    return ROM_DATABASE[checksum] || null;
}

// Handle fullscreen changes
document.addEventListener('fullscreenchange', () => {
    document.body.classList.toggle('fullscreen', !!document.fullscreenElement);
});

// ============================================================================
// Prefs File Handling
// ============================================================================

// Default prefs template - JS handles all config complexity
const PREFS_TEMPLATE = `# Basilisk II preferences - generated by web UI

# ROM file (required)
rom {{ROM_PATH}}

# Disk images
{{DISK_LINES}}

# Hardware settings
ramsize {{RAM_BYTES}}
screen ipc/{{SCREEN_W}}/{{SCREEN_H}}
cpu {{CPU}}
modelid {{MODEL}}
fpu {{FPU}}
jit {{JIT}}
nosound {{NOSOUND}}

# Video codec for web streaming (png or h264)
webcodec {{WEBCODEC}}

# JIT settings
jitfpu true
jitcachesize 8192
jitlazyflush true
jitinline true
jitdebug false

# Display settings
displaycolordepth 0
frameskip 0
scale_nearest false
scale_integer false

# Input settings
keyboardtype 5
keycodes false
mousewheelmode 1
mousewheellines 3
swap_opt_cmd true
hotkey 0

# Serial/Network
seriala /dev/null
serialb /dev/null
udptunnel false
udpport 6066
etherpermanentaddress true
ethermulticastmode 0
routerenabled false
ftp_port_list 21

# Boot settings
bootdrive 0
bootdriver 0
nocdrom false

# System settings
ignoresegv true
idlewait true
noclipconversion false
nogui true
sound_buffer 0
name_encoding 0
delay 0
init_grab false
yearofs 0
dayofs 0
reservewindowskey false

# ExtFS settings
enableextfs false
debugextfs false
extfs ./storage
extdrives CDEFGHIJKLMNOPQRSTUVWXYZ
pollmedia true
`;

// Parse prefs file content into config object
function parsePrefsFile(content) {
    const config = {
        rom: '',
        disks: [],
        ram: 32,
        screen: '800x600',
        webcodec: 'png',
        cpu: 4,
        model: 14,
        fpu: true,
        jit: true,
        sound: true
    };

    if (!content) return config;

    const lines = content.split('\n');
    for (const line of lines) {
        const trimmed = line.trim();
        if (!trimmed || trimmed.startsWith('#')) continue;

        const spaceIdx = trimmed.indexOf(' ');
        if (spaceIdx === -1) continue;

        const key = trimmed.substring(0, spaceIdx);
        const value = trimmed.substring(spaceIdx + 1).trim();

        switch (key) {
            case 'rom':
                // Store path relative to romsPath (strip the base path prefix if present)
                // e.g., "storage/roms/1MB ROMs/foo.ROM" -> "1MB ROMs/foo.ROM"
                // or just "1MB ROMs/foo.ROM" if already relative
                config.rom = value.replace(/^storage\/roms\//, '');
                break;
            case 'disk':
                // Store path relative to imagesPath
                config.disks.push(value.replace(/^storage\/images\//, ''));
                break;
            case 'ramsize':
                config.ram = Math.round(parseInt(value) / (1024 * 1024));
                break;
            case 'screen':
                // Parse "ipc/800/600" format
                const parts = value.split('/');
                if (parts.length >= 3) {
                    config.screen = `${parts[1]}x${parts[2]}`;
                }
                break;
            case 'cpu':
                config.cpu = parseInt(value);
                break;
            case 'modelid':
                config.model = parseInt(value);
                break;
            case 'fpu':
                config.fpu = value === 'true';
                break;
            case 'jit':
                config.jit = value === 'true';
                break;
            case 'nosound':
                config.sound = value !== 'true';
                break;
            case 'webcodec':
                config.webcodec = value === 'h264' ? 'h264' : 'png';
                break;
        }
    }

    return config;
}

// Generate prefs file content from config object
function generatePrefsFile(config, romsPath, imagesPath) {
    // Get absolute paths (server provides base paths)
    const romPath = config.rom ? `${romsPath}/${config.rom}` : '';
    const diskLines = config.disks.map(d => `disk ${imagesPath}/${d}`).join('\n');

    // Parse screen resolution
    const screenMatch = config.screen.match(/(\d+)x(\d+)/);
    const screenW = screenMatch ? screenMatch[1] : '800';
    const screenH = screenMatch ? screenMatch[2] : '600';

    // Apply template
    let prefs = PREFS_TEMPLATE
        .replace('{{ROM_PATH}}', romPath)
        .replace('{{DISK_LINES}}', diskLines || '# No disk images configured')
        .replace('{{RAM_BYTES}}', (config.ram * 1024 * 1024).toString())
        .replace('{{SCREEN_W}}', screenW)
        .replace('{{SCREEN_H}}', screenH)
        .replace('{{CPU}}', config.cpu.toString())
        .replace('{{MODEL}}', config.model.toString())
        .replace('{{FPU}}', config.fpu ? 'true' : 'false')
        .replace('{{JIT}}', config.jit ? 'true' : 'false')
        .replace('{{NOSOUND}}', config.sound ? 'false' : 'true')
        .replace('{{WEBCODEC}}', config.webcodec || 'png');

    return prefs;
}

// ============================================================================
// Configuration Modal
// ============================================================================

let currentConfig = {
    rom: '',
    disks: [],
    ram: 32,
    screen: '800x600',
    webcodec: 'png',
    cpu: 4,
    model: 14,
    fpu: true,
    jit: true,
    sound: true
};

// Paths from server (for generating absolute paths in prefs)
let serverPaths = {
    romsPath: 'storage/roms',
    imagesPath: 'storage/images'
};

// Cache for storage data (roms + disks from single API call)
let storageCache = null;

async function loadStorage() {
    if (storageCache) return storageCache;
    try {
        const res = await fetch(getApiUrl('storage'));
        storageCache = await res.json();
        return storageCache;
    } catch (e) {
        logger.error('Failed to load storage', { error: e.message });
        return null;
    }
}

async function openConfig() {
    const modal = document.getElementById('config-modal');
    if (modal) {
        modal.classList.add('open');
        storageCache = null; // Clear cache to refresh
        // Load current config first so ROM/disk selections are correct
        await loadCurrentConfig();
        // Then load the ROM and disk lists (can run in parallel)
        loadRomList();
        loadDiskList();
    }
}

function closeConfig() {
    const modal = document.getElementById('config-modal');
    if (modal) {
        modal.classList.remove('open');
    }
}

function toggleAdvanced() {
    const toggle = document.querySelector('.advanced-toggle');
    const content = document.getElementById('advanced-settings');
    if (toggle && content) {
        toggle.classList.toggle('open');
        content.classList.toggle('open');
    }
}

async function loadRomList() {
    const select = document.getElementById('cfg-rom');
    if (!select) return;

    try {
        const data = await loadStorage();
        if (!data) {
            select.innerHTML = '<option value="">Failed to load</option>';
            return;
        }

        if (data.roms && data.roms.length > 0) {
            // Sort: recommended first, then by name
            const sortedRoms = data.roms.slice().sort((a, b) => {
                const infoA = getRomInfo(a.checksum);
                const infoB = getRomInfo(b.checksum);
                if (infoA?.recommended && !infoB?.recommended) return -1;
                if (!infoA?.recommended && infoB?.recommended) return 1;
                return a.name.localeCompare(b.name);
            });

            select.innerHTML = sortedRoms.map(rom => {
                const info = getRomInfo(rom.checksum);
                const displayName = info ? info.name : `${rom.name} [${rom.checksum}]`;
                const sizeStr = rom.size ? ` - ${(rom.size / 1024 / 1024).toFixed(1)} MB` : '';
                const recStr = info?.recommended ? ' ★' : '';
                const selected = currentConfig.rom === rom.name ? 'selected' : '';
                return `<option value="${rom.name}" ${selected}>${displayName}${sizeStr}${recStr}</option>`;
            }).join('');

            // Auto-select first recommended ROM if none selected
            if (!currentConfig.rom) {
                const recommended = sortedRoms.find(r => getRomInfo(r.checksum)?.recommended);
                if (recommended) {
                    currentConfig.rom = recommended.name;
                    select.value = recommended.name;
                } else if (sortedRoms.length > 0) {
                    currentConfig.rom = sortedRoms[0].name;
                    select.value = sortedRoms[0].name;
                }
            }
        } else {
            select.innerHTML = '<option value="">No ROM files found</option>';
        }
    } catch (e) {
        select.innerHTML = '<option value="">Failed to load ROMs</option>';
        logger.error('Failed to load ROM list', { error: e.message });
    }
}

async function loadDiskList() {
    const container = document.getElementById('disk-list');
    if (!container) return;

    try {
        const data = await loadStorage();
        if (!data) {
            container.innerHTML = '<div class="empty-state">Failed to load storage</div>';
            return;
        }

        if (data.disks && data.disks.length > 0) {
            container.innerHTML = data.disks.map((disk, idx) => {
                const checked = currentConfig.disks.includes(disk.name) ? 'checked' : '';
                const sizeStr = disk.size ? ` (${(disk.size / 1024 / 1024).toFixed(1)} MB)` : '';
                return `
                    <div class="checkbox-group">
                        <input type="checkbox" id="disk-${idx}" value="${disk.name}" ${checked} onchange="updateDiskSelection()">
                        <label for="disk-${idx}">${disk.name}${sizeStr}</label>
                    </div>`;
            }).join('');
        } else {
            container.innerHTML = '<div class="empty-state">No disk images found in storage/images/</div>';
        }
    } catch (e) {
        container.innerHTML = '<div class="empty-state">Failed to load disks</div>';
        logger.error('Failed to load disk list', { error: e.message });
    }
}

function updateDiskSelection() {
    const checkboxes = document.querySelectorAll('#disk-list input[type="checkbox"]:checked');
    currentConfig.disks = Array.from(checkboxes).map(cb => cb.value);
}

function onRomChange() {
    const romName = document.getElementById('cfg-rom')?.value;
    if (!romName || !storageCache?.roms) return;

    // Find ROM in storage cache
    const rom = storageCache.roms.find(r => r.name === romName);
    if (!rom) return;

    // Look up ROM info and auto-set model if known
    const info = getRomInfo(rom.checksum);
    if (info?.model) {
        const modelSelect = document.getElementById('cfg-model');
        if (modelSelect) {
            modelSelect.value = info.model;
            currentConfig.model = info.model;
        }
    }
}

async function loadCurrentConfig() {
    try {
        const res = await fetch(getApiUrl('prefs'));
        const data = await res.json();

        // Store server paths for later use
        if (data.romsPath) serverPaths.romsPath = data.romsPath;
        if (data.imagesPath) serverPaths.imagesPath = data.imagesPath;

        // Parse prefs file content
        if (data.content) {
            currentConfig = parsePrefsFile(data.content);
            logger.info('Loaded config from prefs file', currentConfig);
        }
        updateConfigUI();
    } catch (e) {
        logger.warn('Failed to load current config', { error: e.message });
    }
}

function updateConfigUI() {
    const romEl = document.getElementById('cfg-rom');
    const ramEl = document.getElementById('cfg-ram');
    const screenEl = document.getElementById('cfg-screen');
    const webcodecEl = document.getElementById('cfg-webcodec');
    const cpuEl = document.getElementById('cfg-cpu');
    const modelEl = document.getElementById('cfg-model');
    const fpuEl = document.getElementById('cfg-fpu');
    const jitEl = document.getElementById('cfg-jit');
    const soundEl = document.getElementById('cfg-sound');

    if (romEl) romEl.value = currentConfig.rom;
    if (ramEl) ramEl.value = currentConfig.ram;
    if (screenEl) screenEl.value = currentConfig.screen;
    if (webcodecEl) webcodecEl.value = currentConfig.webcodec || 'png';
    if (cpuEl) cpuEl.value = currentConfig.cpu;
    if (modelEl) modelEl.value = currentConfig.model;
    if (fpuEl) fpuEl.checked = currentConfig.fpu;
    if (jitEl) jitEl.checked = currentConfig.jit;
    if (soundEl) soundEl.checked = currentConfig.sound;

    // Update disk checkboxes
    document.querySelectorAll('#disk-list input[type="checkbox"]').forEach(cb => {
        cb.checked = currentConfig.disks.includes(cb.value);
    });
}

async function saveConfig() {
    // Gather values from form
    currentConfig.rom = document.getElementById('cfg-rom')?.value || '';
    currentConfig.ram = parseInt(document.getElementById('cfg-ram')?.value || 32);
    currentConfig.screen = document.getElementById('cfg-screen')?.value || '800x600';
    currentConfig.webcodec = document.getElementById('cfg-webcodec')?.value || 'png';
    currentConfig.cpu = parseInt(document.getElementById('cfg-cpu')?.value || 4);
    currentConfig.model = parseInt(document.getElementById('cfg-model')?.value || 14);
    currentConfig.fpu = document.getElementById('cfg-fpu')?.checked ?? true;
    currentConfig.jit = document.getElementById('cfg-jit')?.checked ?? true;
    currentConfig.sound = document.getElementById('cfg-sound')?.checked ?? true;
    // disks already updated via updateDiskSelection()

    // Generate prefs file content
    const prefsContent = generatePrefsFile(currentConfig, serverPaths.romsPath, serverPaths.imagesPath);
    logger.debug('Generated prefs file', { length: prefsContent.length });

    try {
        const res = await fetch(getApiUrl('prefs'), {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ content: prefsContent })
        });
        const data = await res.json();

        if (data.success) {
            logger.info('Configuration saved');
            closeConfig();
            // Restart emulator with new config
            restartEmulator();
        } else {
            logger.error('Failed to save config', { message: data.error });
        }
    } catch (e) {
        logger.error('Failed to save config', { error: e.message });
    }
}

// ============================================================================
// Emulator Control
// ============================================================================

async function startEmulator() {
    logger.info('Starting emulator...');
    try {
        const res = await fetch(getApiUrl('emulator/start'), { method: 'POST' });
        const data = await res.json();
        logger.info('Start emulator', { message: data.message });
    } catch (e) {
        logger.error('Failed to start emulator', { error: e.message });
    }
}

async function stopEmulator() {
    logger.info('Stopping emulator...');
    try {
        const res = await fetch(getApiUrl('emulator/stop'), { method: 'POST' });
        const data = await res.json();
        logger.info('Stop emulator', { message: data.message });
    } catch (e) {
        logger.error('Failed to stop emulator', { error: e.message });
    }
}

async function restartEmulator() {
    logger.info('Restarting emulator...');
    try {
        const res = await fetch(getApiUrl('emulator/restart'), { method: 'POST' });
        const data = await res.json();
        logger.info('Restart emulator', { message: data.message });
    } catch (e) {
        logger.error('Failed to restart emulator', { error: e.message });
    }
}

// Emulator status polling
async function pollEmulatorStatus() {
    try {
        const res = await fetch(getApiUrl('status'));
        const data = await res.json();

        const dotRunning = document.getElementById('dot-running');
        const dotConnected = document.getElementById('dot-connected');
        const emuPid = document.getElementById('emu-pid');

        if (dotRunning) {
            dotRunning.className = 'dot ' + (data.emulator_running ? 'green' : 'red');
        }
        if (dotConnected) {
            dotConnected.className = 'dot ' + (data.emulator_connected ? 'green' : 'red');
        }
        if (emuPid) {
            emuPid.textContent = 'PID: ' + (data.emulator_pid > 0 ? data.emulator_pid : '-');
        }

        // Update mouse latency stat (from emulator via server)
        const mouseLatencyEl = document.getElementById('stat-mouse-latency');
        if (mouseLatencyEl && data.mouse_latency_ms !== undefined) {
            if (data.mouse_latency_samples > 0) {
                mouseLatencyEl.textContent = data.mouse_latency_ms.toFixed(1) + ' ms';
            } else {
                mouseLatencyEl.textContent = '-- ms';
            }
        }

        // Update video latency stat (from PNGDecoder)
        const videoLatencyEl = document.getElementById('stat-video-latency');
        if (videoLatencyEl && client && client.decoder) {
            const decoder = client.decoder;
            if (decoder.getAverageLatency) {
                const avgLatency = decoder.getAverageLatency();
                if (avgLatency > 0) {
                    videoLatencyEl.textContent = avgLatency.toFixed(1) + ' ms';
                } else {
                    videoLatencyEl.textContent = '-- ms';
                }
            }
        }

        // Update RTT stat (from PNGDecoder)
        const rttEl = document.getElementById('stat-rtt');
        if (rttEl && client && client.decoder) {
            const decoder = client.decoder;
            if (decoder.getAverageRtt) {
                const avgRtt = decoder.getAverageRtt();
                if (avgRtt > 0) {
                    rttEl.textContent = avgRtt.toFixed(1) + ' ms';
                } else {
                    rttEl.textContent = '-- ms';
                }
            }
        }

        // Update ping breakdown stats (from latest ping response)
        if (client && client.decoder && client.decoder.getLatestPing) {
            const ping = client.decoder.getLatestPing();
            if (ping) {
                const networkEl = document.getElementById('stat-ping-network');
                const ipcEl = document.getElementById('stat-ping-ipc');
                const waitEl = document.getElementById('stat-ping-wait');
                const encodeEl = document.getElementById('stat-ping-encode');

                if (networkEl) networkEl.textContent = ping.network_ms.toFixed(1) + ' ms';
                if (ipcEl) ipcEl.textContent = ping.ipc_ms.toFixed(1) + ' ms';
                if (waitEl) waitEl.textContent = ping.wait_ms.toFixed(1) + ' ms';
                if (encodeEl) encodeEl.textContent = ping.encode_ms.toFixed(1) + ' ms';
            }
        }
    } catch (e) {
        // Silently fail status polling
    }
}

// Start status polling
setInterval(pollEmulatorStatus, 2000);

// Initialize on page load
window.addEventListener('DOMContentLoaded', async () => {
    await fetchDebugConfig();  // Load debug flags from server
    initClient();
    pollEmulatorStatus();
});
