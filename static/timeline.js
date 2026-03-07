// Timeline Canvas Renderer - chrome://tracing style
var TimelineRenderer = (function() {
  var canvas, ctx, tooltip;
  var data = null;
  var rows = [];

  // View state
  var viewLeft = 0;   // us (start of visible range)
  var viewRight = 0;  // us (end of visible range)
  var scrollY = 0;    // px
  var ROW_H = 24;
  var LABEL_W = 180;
  var dpr = 1;

  // Interaction
  var dragStartX = -1;
  var dragStartViewLeft = 0;
  var dragStartViewRight = 0;
  var hoveredRow = -1;
  var hoveredProc = null;

  // Colors
  var COLORS = [
    '#89b4fa', '#a6e3a1', '#f9e2af', '#fab387', '#f38ba8',
    '#74c7ec', '#94e2d5', '#cba6f7', '#eba0ac', '#b4befe',
    '#89dceb', '#a6adc8'
  ];

  function hashColor(str) {
    var h = 0;
    for (var i = 0; i < str.length; i++) h = ((h << 5) - h + str.charCodeAt(i)) | 0;
    return COLORS[Math.abs(h) % COLORS.length];
  }

  function cmdName(cmdline) {
    var first = cmdline.split(' ')[0];
    var sl = first.lastIndexOf('/');
    return sl >= 0 ? first.substr(sl + 1) : first;
  }

  function formatDuration(us) {
    if (us < 1000) return us + 'us';
    if (us < 1000000) return (us / 1000).toFixed(1) + 'ms';
    return (us / 1000000).toFixed(2) + 's';
  }

  function init(canvasEl, tooltipEl, timelineData) {
    canvas = canvasEl;
    tooltip = tooltipEl;
    data = timelineData;
    rows = data.processes || [];

    dpr = window.devicePixelRatio || 1;

    viewLeft = data.min_time_us;
    viewRight = data.max_time_us;

    resize();
    bindEvents();
    render();
  }

  function resize() {
    var container = canvas.parentElement;
    var w = container.clientWidth;
    var h = container.clientHeight;
    canvas.width = w * dpr;
    canvas.height = h * dpr;
    canvas.style.width = w + 'px';
    canvas.style.height = h + 'px';
    ctx = canvas.getContext('2d');
    ctx.scale(dpr, dpr);
  }

  function render() {
    var w = canvas.clientWidth;
    var h = canvas.clientHeight;
    var totalDur = viewRight - viewLeft;
    if (totalDur <= 0) return;

    ctx.clearRect(0, 0, w, h);

    // Background
    ctx.fillStyle = '#1e1e2e';
    ctx.fillRect(0, 0, w, h);

    // Label column background
    ctx.fillStyle = '#252536';
    ctx.fillRect(0, 0, LABEL_W, h);

    // Time scale header
    ctx.fillStyle = '#2a2a3c';
    ctx.fillRect(0, 0, w, ROW_H);

    // Draw time ticks
    var barW = w - LABEL_W;
    var tickInterval = niceInterval(totalDur, barW / 80);
    var firstTick = Math.ceil(viewLeft / tickInterval) * tickInterval;

    ctx.strokeStyle = '#45475a';
    ctx.fillStyle = '#a6adc8';
    ctx.font = '10px monospace';
    ctx.textAlign = 'center';

    for (var t = firstTick; t <= viewRight; t += tickInterval) {
      var x = LABEL_W + (t - viewLeft) / totalDur * barW;
      // Grid line
      ctx.beginPath();
      ctx.moveTo(x, ROW_H);
      ctx.lineTo(x, h);
      ctx.stroke();
      // Label
      ctx.fillText(formatTime(t - data.min_time_us), x, 14);
    }

    // Draw rows
    var visibleStart = Math.floor(scrollY / ROW_H);
    var visibleEnd = Math.min(rows.length, visibleStart + Math.ceil(h / ROW_H) + 1);

    for (var i = visibleStart; i < visibleEnd; i++) {
      var y = i * ROW_H - scrollY + ROW_H; // +ROW_H for header
      if (y + ROW_H < ROW_H || y > h) continue;

      var proc = rows[i];
      var dur = proc.end_time_us - proc.start_time_us;
      var name = cmdName(proc.cmdline);

      // Row background (alternate)
      if (i % 2 === 0) {
        ctx.fillStyle = '#22223300';
      }

      // Hover highlight
      if (i === hoveredRow) {
        ctx.fillStyle = '#45475a44';
        ctx.fillRect(0, y, w, ROW_H);
      }

      // Label
      var indent = (proc.depth || 0) * 8;
      ctx.fillStyle = '#a6adc8';
      ctx.font = '11px monospace';
      ctx.textAlign = 'left';
      var labelText = name;
      if (indent + ctx.measureText(labelText).width > LABEL_W - 8) {
        while (labelText.length > 3 && indent + ctx.measureText(labelText + '..').width > LABEL_W - 8) {
          labelText = labelText.slice(0, -1);
        }
        labelText += '..';
      }
      ctx.fillText(labelText, 4 + indent, y + ROW_H * 0.7);

      // Bar
      var barStart = LABEL_W + (proc.start_time_us - viewLeft) / totalDur * barW;
      var barEnd = LABEL_W + (proc.end_time_us - viewLeft) / totalDur * barW;
      var bw = barEnd - barStart;
      if (bw < 1) bw = 1;

      // Clip to visible area
      if (barEnd < LABEL_W || barStart > w) continue;

      var color = proc.exit_code !== 0 ? '#f38ba8' : hashColor(name);
      ctx.fillStyle = color;
      ctx.fillRect(Math.max(barStart, LABEL_W), y + 3, Math.min(bw, w - Math.max(barStart, LABEL_W)), ROW_H - 6);

      // Bar label (if wide enough)
      if (bw > 40) {
        ctx.fillStyle = '#1e1e2e';
        ctx.font = '10px monospace';
        var barLabel = name;
        if (ctx.measureText(barLabel).width > bw - 8) {
          while (barLabel.length > 2 && ctx.measureText(barLabel + '..').width > bw - 8) {
            barLabel = barLabel.slice(0, -1);
          }
          barLabel += '..';
        }
        ctx.fillText(barLabel, Math.max(barStart + 4, LABEL_W + 4), y + ROW_H * 0.7);
      }
    }

    // Label column separator
    ctx.strokeStyle = '#45475a';
    ctx.beginPath();
    ctx.moveTo(LABEL_W, 0);
    ctx.lineTo(LABEL_W, h);
    ctx.stroke();

    // Scrollbar
    if (rows.length * ROW_H > h - ROW_H) {
      var totalH = rows.length * ROW_H;
      var viewH = h - ROW_H;
      var sbH = Math.max(20, viewH * viewH / totalH);
      var sbY = ROW_H + scrollY / totalH * viewH;
      ctx.fillStyle = '#45475a88';
      ctx.fillRect(w - 8, sbY, 6, sbH);
    }
  }

  function niceInterval(range, minPixels) {
    var target = range * minPixels / ((canvas.clientWidth - LABEL_W) || 1);
    var intervals = [1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000,
      10000, 20000, 50000, 100000, 200000, 500000, 1000000, 2000000, 5000000,
      10000000, 20000000, 50000000, 100000000];
    for (var i = 0; i < intervals.length; i++) {
      if (intervals[i] >= target) return intervals[i];
    }
    return intervals[intervals.length - 1];
  }

  function formatTime(us) {
    if (us < 1000) return us + 'us';
    if (us < 1000000) return (us / 1000).toFixed(1) + 'ms';
    return (us / 1000000).toFixed(2) + 's';
  }

  function bindEvents() {
    canvas.addEventListener('wheel', function(e) {
      e.preventDefault();
      if (e.ctrlKey || e.metaKey) {
        // Horizontal zoom
        var rect = canvas.getBoundingClientRect();
        var mx = e.clientX - rect.left;
        var frac = (mx - LABEL_W) / (canvas.clientWidth - LABEL_W);
        frac = Math.max(0, Math.min(1, frac));

        var center = viewLeft + (viewRight - viewLeft) * frac;
        var factor = e.deltaY > 0 ? 1.2 : 1 / 1.2;
        var newRange = (viewRight - viewLeft) * factor;
        var minRange = 100; // 100us minimum
        if (newRange < minRange) newRange = minRange;
        var maxRange = data.max_time_us - data.min_time_us;
        if (newRange > maxRange) newRange = maxRange;

        viewLeft = center - newRange * frac;
        viewRight = center + newRange * (1 - frac);
        if (viewLeft < data.min_time_us) { viewLeft = data.min_time_us; viewRight = viewLeft + newRange; }
        if (viewRight > data.max_time_us) { viewRight = data.max_time_us; viewLeft = viewRight - newRange; }
      } else {
        // Vertical scroll
        var maxScroll = Math.max(0, rows.length * ROW_H - (canvas.clientHeight - ROW_H));
        scrollY = Math.max(0, Math.min(maxScroll, scrollY + e.deltaY));
      }
      render();
    });

    canvas.addEventListener('mousedown', function(e) {
      var rect = canvas.getBoundingClientRect();
      var mx = e.clientX - rect.left;
      if (mx > LABEL_W) {
        dragStartX = mx;
        dragStartViewLeft = viewLeft;
        dragStartViewRight = viewRight;
        canvas.style.cursor = 'grabbing';
      }
    });

    canvas.addEventListener('mousemove', function(e) {
      var rect = canvas.getBoundingClientRect();
      var mx = e.clientX - rect.left;
      var my = e.clientY - rect.top;

      if (dragStartX >= 0) {
        var dx = mx - dragStartX;
        var barW = canvas.clientWidth - LABEL_W;
        var range = dragStartViewRight - dragStartViewLeft;
        var shift = -dx / barW * range;
        viewLeft = dragStartViewLeft + shift;
        viewRight = dragStartViewRight + shift;
        var maxRange = data.max_time_us - data.min_time_us;
        if (viewLeft < data.min_time_us) { viewLeft = data.min_time_us; viewRight = viewLeft + range; }
        if (viewRight > data.max_time_us) { viewRight = data.max_time_us; viewLeft = viewRight - range; }
        render();
        return;
      }

      // Hover
      var rowIdx = Math.floor((my - ROW_H + scrollY) / ROW_H);
      if (rowIdx >= 0 && rowIdx < rows.length && my > ROW_H) {
        hoveredRow = rowIdx;
        hoveredProc = rows[rowIdx];
        var dur = hoveredProc.end_time_us - hoveredProc.start_time_us;
        tooltip.style.display = 'block';
        tooltip.innerHTML =
          '<div class="tt-pid">PID: ' + hoveredProc.pid + '</div>' +
          '<div class="tt-cmd">' + escapeHtml(hoveredProc.cmdline) + '</div>' +
          '<div class="tt-dur">Duration: ' + formatDuration(dur) + '</div>' +
          '<div class="tt-exit' + (hoveredProc.exit_code !== 0 ? ' err' : '') + '">Exit: ' + hoveredProc.exit_code + '</div>';

        var tx = e.clientX - canvas.parentElement.getBoundingClientRect().left + 12;
        var ty = e.clientY - canvas.parentElement.getBoundingClientRect().top + 12;
        if (tx + 300 > canvas.clientWidth) tx = tx - 320;
        tooltip.style.left = tx + 'px';
        tooltip.style.top = ty + 'px';
      } else {
        hoveredRow = -1;
        hoveredProc = null;
        tooltip.style.display = 'none';
      }
      render();
    });

    canvas.addEventListener('mouseup', function() {
      dragStartX = -1;
      canvas.style.cursor = '';
    });

    canvas.addEventListener('mouseleave', function() {
      dragStartX = -1;
      hoveredRow = -1;
      tooltip.style.display = 'none';
      canvas.style.cursor = '';
      render();
    });

    canvas.addEventListener('dblclick', function(e) {
      var rect = canvas.getBoundingClientRect();
      var my = e.clientY - rect.top;
      var rowIdx = Math.floor((my - ROW_H + scrollY) / ROW_H);
      if (rowIdx >= 0 && rowIdx < rows.length) {
        var proc = rows[rowIdx];
        var dur = proc.end_time_us - proc.start_time_us;
        var padding = dur * 0.1;
        viewLeft = proc.start_time_us - padding;
        viewRight = proc.end_time_us + padding;
        if (viewLeft < data.min_time_us) viewLeft = data.min_time_us;
        if (viewRight > data.max_time_us) viewRight = data.max_time_us;
        render();
      }
    });

    window.addEventListener('resize', function() {
      resize();
      render();
    });
  }

  function escapeHtml(s) {
    return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
  }

  return { init: init };
})();
