// bdtrace Web UI - app.js

var App = (function() {
  var currentView = 'summary';
  var cache = {};
  var viewDom = {};

  // Persistent column visibility for process tree
  var procColumns = {
    files: true, duration: true, pctTime: true, start: false, end: false,
    cpu: true, pctCpu: true, rss: true, io: true, fails: true, exit: true
  };

  function api(path, cb) {
    if (cache[path]) { cb(cache[path]); return; }
    var xhr = new XMLHttpRequest();
    xhr.open('GET', path);
    xhr.onload = function() {
      if (xhr.status === 200) {
        var data = JSON.parse(xhr.responseText);
        cache[path] = data;
        cb(data);
      }
    };
    xhr.send();
  }

  function el(tag, attrs, children) {
    var e = document.createElement(tag);
    if (attrs) for (var k in attrs) {
      if (k === 'className') e.className = attrs[k];
      else if (k === 'onclick') e.onclick = attrs[k];
      else if (k === 'innerHTML') e.innerHTML = attrs[k];
      else e.setAttribute(k, attrs[k]);
    }
    if (typeof children === 'string') e.textContent = children;
    else if (children) for (var i = 0; i < children.length; i++) {
      if (children[i]) e.appendChild(typeof children[i] === 'string' ? document.createTextNode(children[i]) : children[i]);
    }
    return e;
  }

  function formatDuration(us) {
    if (us < 0) us = 0;
    if (us < 1000) return us + 'us';
    if (us < 1000000) return (us / 1000).toFixed(1) + 'ms';
    return (us / 1000000).toFixed(2) + 's';
  }

  function formatRelSec(us) {
    return (us / 1000000).toFixed(2) + 's';
  }

  function formatBytes(b) {
    if (b <= 0) return '-';
    if (b < 1024) return b + 'B';
    if (b < 1048576) return (b / 1024).toFixed(1) + 'KB';
    if (b < 1073741824) return (b / 1048576).toFixed(1) + 'MB';
    return (b / 1073741824).toFixed(2) + 'GB';
  }

  function formatRss(kb) {
    if (kb <= 0) return '-';
    if (kb < 1024) return kb + 'KB';
    return (kb / 1024).toFixed(1) + 'MB';
  }

  function formatPct(val, total) {
    if (total <= 0 || val <= 0) return '-';
    var pct = val * 100 / total;
    if (pct < 0.1) return '<0.1%';
    if (pct >= 100) return '100%';
    return pct.toFixed(1) + '%';
  }

  function shortenCmd(cmd, max) {
    if (!max || cmd.length <= max) return cmd;
    return cmd.substr(0, max - 3) + '...';
  }

  // Create a span with title tooltip if text is truncated
  function spanWithTitle(text, fullText, className) {
    var attrs = {};
    if (className) attrs.className = className;
    if (fullText && fullText !== text) attrs.title = fullText;
    return el('span', attrs, text);
  }

  // ---- Column resize ----
  function enableColumnResize(table) {
    table.style.tableLayout = 'fixed';
    var ths = table.querySelectorAll('thead th');
    if (!ths.length) return;

    // Set initial widths from computed sizes, then fix them
    for (var i = 0; i < ths.length; i++) {
      ths[i].style.width = ths[i].offsetWidth + 'px';
    }

    for (var i = 0; i < ths.length - 1; i++) {
      (function(th, idx) {
        var handle = document.createElement('span');
        handle.className = 'col-resize-handle';
        th.style.position = 'relative';
        th.appendChild(handle);

        handle.addEventListener('mousedown', function(e) {
          e.preventDefault();
          e.stopPropagation();
          var startX = e.clientX;
          var startW = th.offsetWidth;
          var nextTh = ths[idx + 1];
          var nextW = nextTh.offsetWidth;

          function onMove(e2) {
            var dx = e2.clientX - startX;
            var newW = Math.max(30, startW + dx);
            var newNext = Math.max(30, nextW - dx);
            th.style.width = newW + 'px';
            nextTh.style.width = newNext + 'px';
          }
          function onUp() {
            document.removeEventListener('mousemove', onMove);
            document.removeEventListener('mouseup', onUp);
            document.body.style.cursor = '';
            document.body.style.userSelect = '';
          }
          document.body.style.cursor = 'col-resize';
          document.body.style.userSelect = 'none';
          document.addEventListener('mousemove', onMove);
          document.addEventListener('mouseup', onUp);
        });
      })(ths[i], i);
    }
  }

  function cmdName(cmdline) {
    var first = cmdline.split(' ')[0];
    var sl = first.lastIndexOf('/');
    return sl >= 0 ? first.substr(sl + 1) : first;
  }

  // ---- Tree control helpers ----
  function expandAll(registry) {
    var prev = 0;
    while (registry.length > prev) {
      prev = registry.length;
      for (var i = 0; i < registry.length; i++) {
        if (registry[i].hasChildren) registry[i].expand();
      }
    }
  }

  function collapseAll(registry) {
    for (var i = registry.length - 1; i >= 0; i--) {
      if (registry[i].hasChildren) registry[i].collapse();
    }
  }

  function makeTreeToolbar(registry, extras) {
    var bar = el('div', {style: 'display:flex;gap:8px;margin-bottom:8px;flex-wrap:wrap;align-items:center'});
    bar.appendChild(el('span', {
      className: 'tree-btn',
      onclick: function() { expandAll(registry); }
    }, 'Expand All'));
    bar.appendChild(el('span', {
      className: 'tree-btn',
      onclick: function() { collapseAll(registry); }
    }, 'Collapse All'));
    if (extras) for (var i = 0; i < extras.length; i++) bar.appendChild(extras[i]);
    return bar;
  }

  // ---- Summary View ----
  function renderSummary() {
    var app = document.getElementById('app');
    app.innerHTML = '<div class="loading">Loading...</div>';

    api('/api/summary', function(s) {
      app.innerHTML = '';

      var row = el('div', {className: 'kpi-row'}, [
        kpiCard('Total Time', formatDuration(s.total_time_us)),
        kpiCard('Processes', s.process_count),
        kpiCard('Max Parallelism', s.max_parallelism),
        kpiCard('File Accesses', s.file_access_count + ' (R:' + s.read_count + ' W:' + s.write_count + ')'),
        s.failed_count > 0 ? kpiCard('Failed', s.failed_count) : null
      ]);
      app.appendChild(row);

      if (s.slowest && s.slowest.length > 0) {
        app.appendChild(el('div', {className: 'section-title'}, 'Slowest Processes'));
        var tbl = makeTable(['PID', 'Duration', 'Command'], s.slowest.map(function(p) {
          return [p.pid, formatDuration(p.duration_us), p.cmdline];
        }));
        app.appendChild(tbl);
      }

      api('/api/parallelism', function(p) {
        if (p.buckets && p.buckets.length > 0) {
          app.appendChild(el('div', {className: 'section-title', style: 'margin-top:24px'}, 'Parallelism Over Time'));
          var canvas = el('canvas', {width: '800', height: '200'});
          canvas.style.width = '100%';
          canvas.style.maxWidth = '800px';
          canvas.style.height = '200px';
          canvas.style.background = 'var(--bg2)';
          canvas.style.borderRadius = '6px';
          app.appendChild(canvas);
          drawParallelism(canvas, p);
        }
      });
    });
  }

  function kpiCard(label, value) {
    return el('div', {className: 'kpi'}, [
      el('div', {className: 'label'}, label),
      el('div', {className: 'value'}, String(value))
    ]);
  }

  function makeTable(headers, rows) {
    var tbl = el('table');
    var thead = el('tr');
    var sortCol = -1, sortAsc = true;

    for (var h = 0; h < headers.length; h++) {
      (function(idx) {
        var th = el('th', {onclick: function() {
          if (sortCol === idx) sortAsc = !sortAsc;
          else { sortCol = idx; sortAsc = true; }
          rows.sort(function(a, b) {
            var va = a[idx], vb = b[idx];
            if (typeof va === 'number' && typeof vb === 'number') return sortAsc ? va - vb : vb - va;
            va = String(va); vb = String(vb);
            return sortAsc ? va.localeCompare(vb) : vb.localeCompare(va);
          });
          renderBody();
        }}, headers[idx]);
        thead.appendChild(th);
      })(h);
    }
    tbl.appendChild(el('thead', null, [thead]));

    var tbody = el('tbody');
    tbl.appendChild(tbody);

    function renderBody() {
      tbody.innerHTML = '';
      for (var r = 0; r < rows.length; r++) {
        var tr = el('tr');
        for (var c = 0; c < rows[r].length; c++) {
          var isNum = typeof rows[r][c] === 'number';
          tr.appendChild(el('td', isNum ? {className: 'num'} : null, String(rows[r][c])));
        }
        tbody.appendChild(tr);
      }
    }
    renderBody();
    return tbl;
  }

  function drawParallelism(canvas, data) {
    var ctx = canvas.getContext('2d');
    var dpr = window.devicePixelRatio || 1;
    var w = canvas.clientWidth, h = canvas.clientHeight;
    canvas.width = w * dpr;
    canvas.height = h * dpr;
    ctx.scale(dpr, dpr);

    var buckets = data.buckets;
    var maxVal = data.max_parallelism || 1;
    var pad = {top: 20, right: 20, bottom: 30, left: 40};
    var cw = w - pad.left - pad.right;
    var ch = h - pad.top - pad.bottom;
    var barW = cw / buckets.length;

    ctx.fillStyle = '#89b4fa';
    for (var i = 0; i < buckets.length; i++) {
      var bh = (buckets[i] / maxVal) * ch;
      ctx.fillRect(pad.left + i * barW, pad.top + ch - bh, Math.max(barW - 1, 1), bh);
    }

    ctx.strokeStyle = '#45475a';
    ctx.beginPath();
    ctx.moveTo(pad.left, pad.top);
    ctx.lineTo(pad.left, pad.top + ch);
    ctx.lineTo(pad.left + cw, pad.top + ch);
    ctx.stroke();

    ctx.fillStyle = '#a6adc8';
    ctx.font = '10px monospace';
    ctx.textAlign = 'right';
    ctx.fillText(maxVal, pad.left - 4, pad.top + 10);
    ctx.fillText('0', pad.left - 4, pad.top + ch);
    ctx.textAlign = 'center';
    ctx.fillText('0s', pad.left, pad.top + ch + 14);
    var totalSec = (data.total_time_us / 1000000).toFixed(1);
    ctx.fillText(totalSec + 's', pad.left + cw, pad.top + ch + 14);
  }

  // ---- Process Explorer ----
  function syncColumnClasses() {
    for (var key in procColumns) {
      document.body.classList.toggle('hide-col-' + key, !procColumns[key]);
    }
  }

  // Column selector UI - toggles visibility of [data-col] elements via body class
  function makeColumnSelector() {
    var btn = el('span', {className: 'tree-btn', style: 'position:relative'}, 'Columns');
    var menu = el('div', {className: 'col-menu', style: 'display:none'});
    var colDefs = [
      ['files','Files'], ['duration','Duration'], ['pctTime','%Time'],
      ['start','Start'], ['end','End'], ['cpu','CPU'], ['pctCpu','%CPU'],
      ['rss','RSS'], ['io','I/O'], ['fails','Fails'], ['exit','Exit']
    ];
    for (var i = 0; i < colDefs.length; i++) {
      (function(key, label) {
        var lbl = el('label');
        var cb = el('input', {type: 'checkbox'});
        cb.checked = !!procColumns[key];
        cb.onchange = function() {
          procColumns[key] = cb.checked;
          document.body.classList.toggle('hide-col-' + key, !cb.checked);
        };
        lbl.appendChild(cb);
        lbl.appendChild(document.createTextNode(' ' + label));
        menu.appendChild(lbl);
      })(colDefs[i][0], colDefs[i][1]);
    }
    btn.onclick = function(e) {
      e.stopPropagation();
      menu.style.display = menu.style.display === 'none' ? 'block' : 'none';
    };
    document.addEventListener('click', function() { menu.style.display = 'none'; });
    menu.onclick = function(e) { e.stopPropagation(); };

    var wrap = el('span', {style: 'position:relative;display:inline-block'});
    wrap.appendChild(btn);
    wrap.appendChild(menu);
    return wrap;
  }

  function renderProcesses() {
    if (viewDom['processes']) {
      var app = document.getElementById('app');
      app.innerHTML = '';
      app.appendChild(viewDom['processes']);
      return;
    }

    var app = document.getElementById('app');
    app.innerHTML = '<div class="loading">Loading...</div>';

    api('/api/processes', function(data) {
      app.innerHTML = '';
      var wrapper = el('div');
      viewDom['processes'] = wrapper;
      buildProcessView(wrapper, data);
      app.appendChild(wrapper);
    });
  }

  function buildProcessView(wrapper, data) {
    wrapper.innerHTML = '';
    var split = el('div', {className: 'split'});
    var left = el('div', {className: 'left'});
    var right = el('div', {className: 'right'});

    left.appendChild(el('div', {className: 'section-title'}, 'Process Tree'));
    right.appendChild(el('div', {className: 'section-title', id: 'proc-detail-title'}, 'Select a process'));

    var totalDur = 0;
    var totalCpu = 0;
    var minStart = 0;
    var procs = data.processes;
    var procMap = {};
    for (var i = 0; i < procs.length; i++) {
      procMap[procs[i].pid] = procs[i];
      var d = procs[i].end_time_us - procs[i].start_time_us;
      if (d > totalDur) totalDur = d;
      totalCpu += (procs[i].user_time_us || 0) + (procs[i].sys_time_us || 0);
      if (i === 0 || procs[i].start_time_us < minStart) minStart = procs[i].start_time_us;
    }

    var nodeRegistry = [];
    var ctx = { procMap: procMap, childMap: data.children_map, totalDur: totalDur, totalCpu: totalCpu, minStart: minStart };

    // Toolbar with column selector
    var colSelector = makeColumnSelector();
    left.appendChild(makeTreeToolbar(nodeRegistry, [colSelector]));

    // Tree header - always create all, use data-col for toggling
    var hdr = el('div', {className: 'tree-header'});
    hdr.appendChild(el('span', {className: 'th-tree'}, 'Process'));
    var hdrColsWrap = el('span', {className: 'node-cols'});
    var hdrCols = [
      ['files', 'th-col', 'Files'],
      ['fails', 'th-col', 'Fails'],
      ['duration', 'th-col th-dur', 'Duration'],
      ['pctTime', 'th-col', '%Time'],
      ['start', 'th-col', 'Start'],
      ['end', 'th-col', 'End'],
      ['cpu', 'th-col', 'CPU'],
      ['pctCpu', 'th-col', '%CPU'],
      ['rss', 'th-col', 'RSS'],
      ['io', 'th-col', 'I/O'],
      ['exit', 'th-col th-exit', 'Exit']
    ];
    for (var hi = 0; hi < hdrCols.length; hi++) {
      var hc = el('span', {className: hdrCols[hi][1], 'data-col': hdrCols[hi][0]}, hdrCols[hi][2]);
      hdrColsWrap.appendChild(hc);
    }
    hdr.appendChild(hdrColsWrap);

    var treeDiv = el('div', {className: 'tree'});
    treeDiv.appendChild(hdr);
    var roots = data.roots || [];
    var rootUl = el('ul');
    for (var r = 0; r < roots.length; r++) {
      rootUl.appendChild(buildTreeNode(roots[r], ctx, right, nodeRegistry, 0));
    }
    treeDiv.appendChild(rootUl);
    left.appendChild(treeDiv);

    split.appendChild(left);
    split.appendChild(right);
    wrapper.appendChild(split);
  }

  function buildTreeNode(pid, ctx, rightPanel, registry, depth) {
    var proc = ctx.procMap[pid];
    if (!proc) return el('li');
    var dur = proc.end_time_us - proc.start_time_us;
    var children = ctx.childMap[pid] || [];
    var hasChildren = children.length > 0;
    var expanded = false;

    var li = el('li');
    var node = el('div', {className: 'node'});
    var toggle = el('span', {className: 'toggle'}, hasChildren ? '+' : ' ');
    var pidSpan = el('span', {className: 'pid'}, '[' + pid + ']');
    var cmdText = cmdName(proc.cmdline);
    var cmdSpan = spanWithTitle(cmdText, proc.cmdline, 'cmd' + (proc.exit_code !== 0 ? ' exit-err' : ''));

    var treePart = el('span', {className: 'node-tree'});
    if (depth > 0) treePart.style.paddingLeft = (depth * 16) + 'px';
    treePart.appendChild(toggle);
    treePart.appendChild(pidSpan);
    treePart.appendChild(cmdSpan);
    node.appendChild(treePart);

    var cpuTotal = (proc.user_time_us || 0) + (proc.sys_time_us || 0);
    var ioTotal = (proc.io_read_bytes || 0) + (proc.io_write_bytes || 0);
    var fc = proc.fail_count || 0;

    var colsPart = el('span', {className: 'node-cols'});
    var cols = [
      ['files', 'col-num', proc.file_count != null ? String(proc.file_count) : '-'],
      ['fails', 'col-num' + (fc > 0 ? ' exit-err' : ''), fc > 0 ? String(fc) : '-'],
      ['duration', 'col-num col-dur' + (dur > ctx.totalDur * 0.25 ? ' slow' : ''), formatDuration(dur)],
      ['pctTime', 'col-num', formatPct(dur, ctx.totalDur)],
      ['start', 'col-num', formatRelSec(proc.start_time_us - ctx.minStart)],
      ['end', 'col-num', formatRelSec(proc.end_time_us - ctx.minStart)],
      ['cpu', 'col-num', cpuTotal > 0 ? formatDuration(cpuTotal) : '-'],
      ['pctCpu', 'col-num', formatPct(cpuTotal, ctx.totalCpu)],
      ['rss', 'col-num', formatRss(proc.peak_rss_kb || 0)],
      ['io', 'col-num', ioTotal > 0 ? formatBytes(ioTotal) : '-'],
      ['exit', 'col-num col-exit' + (proc.exit_code !== 0 ? ' exit-err' : ''), String(proc.exit_code)]
    ];
    for (var ci = 0; ci < cols.length; ci++) {
      var sp = el('span', {className: cols[ci][1], 'data-col': cols[ci][0]}, cols[ci][2]);
      colsPart.appendChild(sp);
    }
    node.appendChild(colsPart);

    var childUl = null;

    function ensureChildren() {
      if (!childUl && hasChildren) {
        childUl = el('ul');
        // Virtual "(process only)" node to show self-only file accesses
        var selfLi = el('li');
        var selfNode = el('div', {className: 'node'});
        var selfTree = el('span', {className: 'node-tree'});
        if (depth + 1 > 0) selfTree.style.paddingLeft = ((depth + 1) * 16) + 'px';
        selfTree.appendChild(el('span', {className: 'toggle'}, ' '));
        selfTree.appendChild(el('span', {className: 'cmd', style: 'font-style:italic;color:var(--fg2)'}, '(process only)'));
        selfNode.appendChild(selfTree);
        selfNode.onclick = function(e) {
          e.stopPropagation();
          var prev = document.querySelector('.tree .node.selected');
          if (prev) prev.className = prev.className.replace(' selected', '');
          selfNode.className += ' selected';
          showProcessDetail(pid, proc, rightPanel, ctx.minStart, true);
        };
        selfLi.appendChild(selfNode);
        childUl.appendChild(selfLi);
        for (var c = 0; c < children.length; c++) {
          childUl.appendChild(buildTreeNode(children[c], ctx, rightPanel, registry, depth + 1));
        }
        li.appendChild(childUl);
      }
    }

    function doExpand() {
      if (!hasChildren || expanded) return;
      expanded = true;
      toggle.textContent = '-';
      ensureChildren();
      childUl.style.display = '';
    }

    function doCollapse() {
      if (!hasChildren || !expanded) return;
      expanded = false;
      toggle.textContent = '+';
      if (childUl) childUl.style.display = 'none';
    }

    if (registry) {
      registry.push({ expand: doExpand, collapse: doCollapse, hasChildren: hasChildren });
    }

    node.onclick = function(e) {
      e.stopPropagation();
      var prev = document.querySelector('.tree .node.selected');
      if (prev) prev.className = prev.className.replace(' selected', '');
      node.className += ' selected';
      showProcessDetail(pid, proc, rightPanel, ctx.minStart);
      if (hasChildren) {
        if (expanded) doCollapse(); else doExpand();
      }
    };

    li.appendChild(node);
    return li;
  }

  function showProcessDetail(pid, proc, panel, minStart, selfOnly) {
    var titleEl = document.getElementById('proc-detail-title');
    var suffix = selfOnly ? ' (process only)' : '';
    if (titleEl) titleEl.textContent = '[' + pid + '] ' + shortenCmd(proc.cmdline, 60) + suffix;

    var old = panel.querySelector('.proc-files');
    if (old) old.remove();

    var container = el('div', {className: 'proc-files'});
    container.appendChild(renderProcInfo(proc, minStart));
    container.appendChild(el('div', {className: 'loading'}, 'Loading files...'));
    panel.appendChild(container);

    var url = '/api/processes/' + pid + '/files';
    if (!selfOnly) url += '?tree=1';
    api(url, function(files) {
      container.innerHTML = '';
      container.appendChild(renderProcInfo(proc, minStart));

      if (files.length === 0) {
        container.appendChild(el('div', {style: 'color:var(--fg2)'}, 'No file accesses'));
        return;
      }

      var fileMap = {};
      for (var i = 0; i < files.length; i++) {
        var f = files[i];
        if (!fileMap[f.filename]) {
          fileMap[f.filename] = {modes: {}, count: 0, fds: {}};
        }
        fileMap[f.filename].count++;
        fileMap[f.filename].modes[f.mode_str] = true;
        if (f.fd >= 0) fileMap[f.filename].fds[f.fd] = true;
      }

      var absTree = {}, relTree = {};
      var filenames = Object.keys(fileMap);
      for (var i = 0; i < filenames.length; i++) {
        var fname = filenames[i];
        var isAbs = fname.charAt(0) === '/';
        var parts = fname.split('/').filter(function(p) { return p !== ''; });
        var node = isAbs ? absTree : relTree;
        for (var p = 0; p < parts.length; p++) {
          if (!node[parts[p]]) node[parts[p]] = {_info: null, _children: {}};
          if (p === parts.length - 1) {
            node[parts[p]]._info = fileMap[fname];
          } else {
            node = node[parts[p]]._children;
          }
        }
      }

      var tree = {};
      if (Object.keys(absTree).length > 0) {
        tree['/'] = {_info: null, _children: absTree};
      }
      var relKeys = Object.keys(relTree);
      for (var i = 0; i < relKeys.length; i++) {
        tree[relKeys[i]] = relTree[relKeys[i]];
      }

      // Toolbar for proc file tree
      var pfRegistry = [];
      container.appendChild(makeTreeToolbar(pfRegistry));

      var fileHdr = el('div', {className: 'tree-header'});
      fileHdr.appendChild(el('span', {className: 'th-tree'}, 'File'));
      fileHdr.appendChild(el('span', {className: 'th-col'}, 'Mode'));
      fileHdr.appendChild(el('span', {className: 'th-col th-exit'}, 'Count'));
      container.appendChild(fileHdr);

      var treeDiv = el('div', {className: 'tree'});
      treeDiv.appendChild(buildProcFileTree(tree, pfRegistry, 0));
      container.appendChild(treeDiv);

      container.appendChild(el('div', {style: 'margin-top:8px;color:var(--fg2);font-size:11px'}, filenames.length + ' unique files, ' + files.length + ' total accesses'));
    });
  }

  // Shared proc info renderer (used in both Processes detail and Files detail)
  function renderProcInfo(proc, minStart) {
    var frag = el('div');
    frag.appendChild(el('div', {style: 'margin-bottom:4px;color:var(--fg2);word-break:break-all', title: proc.cmdline}, proc.cmdline));

    var line1 = el('div', {style: 'margin-bottom:4px;color:var(--fg2)'});
    var exitSpan = el('span', {'data-col': 'exit'}, [
      el('span', null, 'Exit: '),
      el('span', {className: proc.exit_code === 0 ? '' : 'exit-err'}, String(proc.exit_code)),
      el('span', null, '  ')
    ]);
    line1.appendChild(exitSpan);

    var durSpan = el('span', {'data-col': 'duration'}, 'Duration: ' + formatDuration(proc.end_time_us - proc.start_time_us) + '  ');
    line1.appendChild(durSpan);

    if (minStart != null && proc.start_time_us != null) {
      var startSpan = el('span', {'data-col': 'start'}, 'Start: ' + formatRelSec(proc.start_time_us - minStart) + '  ');
      line1.appendChild(startSpan);
      var endSpan = el('span', {'data-col': 'end'}, 'End: ' + formatRelSec(proc.end_time_us - minStart));
      line1.appendChild(endSpan);
    }
    frag.appendChild(line1);

    var cpuUser = proc.user_time_us || 0, cpuSys = proc.sys_time_us || 0;
    var line2 = el('div', {style: 'margin-bottom:8px;color:var(--fg2)'});
    var hasCpu = cpuUser > 0 || cpuSys > 0;
    var hasRss = (proc.peak_rss_kb || 0) > 0;

    if (hasCpu) {
      var cpuSpan = el('span', {'data-col': 'cpu'}, 'CPU: ' + formatDuration(cpuUser) + ' user, ' + formatDuration(cpuSys) + ' sys  ');
      line2.appendChild(cpuSpan);
    }

    if (hasRss) {
      var rssSpan = el('span', {'data-col': 'rss'}, 'RSS: ' + formatRss(proc.peak_rss_kb || 0) + '  ');
      line2.appendChild(rssSpan);
    }

    var ioSpan = el('span', {'data-col': 'io'}, 'I/O: R:' + formatBytes(proc.io_read_bytes || 0) + ' W:' + formatBytes(proc.io_write_bytes || 0));
    line2.appendChild(ioSpan);

    if (hasCpu || hasRss) frag.appendChild(line2);
    return frag;
  }

  function buildProcFileTree(tree, registry, depth) {
    var ul = el('ul');
    var keys = Object.keys(tree).sort();
    for (var k = 0; k < keys.length; k++) {
      (function(name) {
        var entry = tree[name];
        var hasChildren = Object.keys(entry._children).length > 0;
        var info = entry._info;

        var li = el('li');
        var node = el('div', {className: 'node'});
        if (depth > 0) node.style.paddingLeft = (depth * 16 + 6) + 'px';
        var toggle = el('span', {className: 'toggle'}, hasChildren ? '+' : ' ');
        var nameSpan = el('span', {className: 'cmd'}, name);
        node.appendChild(toggle);
        node.appendChild(nameSpan);

        if (info) {
          var modes = Object.keys(info.modes).join(',');
          node.appendChild(el('span', {className: 'col-num'}, modes));
          node.appendChild(el('span', {className: 'col-num col-exit'}, info.count > 1 ? 'x' + info.count : '1'));
        }

        var childUl = null;
        var expanded = false;

        function ensureChildren() {
          if (!childUl && hasChildren) {
            childUl = buildProcFileTree(entry._children, registry, depth + 1);
            li.appendChild(childUl);
          }
        }

        function doExpand() {
          if (!hasChildren || expanded) return;
          expanded = true;
          toggle.textContent = '-';
          ensureChildren();
          childUl.style.display = '';
        }

        function doCollapse() {
          if (!hasChildren || !expanded) return;
          expanded = false;
          toggle.textContent = '+';
          if (childUl) childUl.style.display = 'none';
        }

        if (registry) {
          registry.push({ expand: doExpand, collapse: doCollapse, hasChildren: hasChildren });
        }

        node.onclick = function(e) {
          e.stopPropagation();
          if (hasChildren) {
            if (expanded) doCollapse(); else doExpand();
          }
        };

        li.appendChild(node);
        ul.appendChild(li);
      })(keys[k]);
    }
    return ul;
  }

  // ---- File Explorer ----
  function renderFiles() {
    if (viewDom['files']) {
      var app = document.getElementById('app');
      app.innerHTML = '';
      app.appendChild(viewDom['files']);
      return;
    }

    var app = document.getElementById('app');
    app.innerHTML = '<div class="loading">Loading...</div>';

    api('/api/files', function(data) {
      app.innerHTML = '';
      var wrapper = el('div');
      var split = el('div', {className: 'split'});
      var left = el('div', {className: 'left'});
      var right = el('div', {className: 'right'});

      left.appendChild(el('div', {className: 'section-title'}, 'File Tree'));
      right.appendChild(el('div', {className: 'section-title', id: 'file-detail-title'}, 'Select a file'));

      var absTree = {};
      var relTree = {};
      for (var i = 0; i < data.length; i++) {
        var path = data[i].path;
        var isAbsolute = path.charAt(0) === '/';
        var parts = path.split('/').filter(function(p) { return p !== ''; });
        var node = isAbsolute ? absTree : relTree;
        for (var p = 0; p < parts.length; p++) {
          if (!node[parts[p]]) node[parts[p]] = {_count: 0, _children: {}};
          node[parts[p]]._count += data[i].access_count;
          if (p < parts.length - 1) node = node[parts[p]]._children;
        }
      }

      var tree = {};
      if (Object.keys(absTree).length > 0) {
        var absTotal = 0;
        var absKeys = Object.keys(absTree);
        for (var i = 0; i < absKeys.length; i++) absTotal += absTree[absKeys[i]]._count;
        tree['/'] = {_count: absTotal, _children: absTree};
      }
      var relKeys = Object.keys(relTree);
      for (var i = 0; i < relKeys.length; i++) {
        tree[relKeys[i]] = relTree[relKeys[i]];
      }

      var nodeRegistry = [];
      left.appendChild(makeTreeToolbar(nodeRegistry));

      var treeDiv = el('div', {className: 'tree'});
      treeDiv.appendChild(buildFileTree(tree, '', right, nodeRegistry, 0));
      left.appendChild(treeDiv);

      split.appendChild(left);
      split.appendChild(right);
      wrapper.appendChild(split);
      app.appendChild(wrapper);
      viewDom['files'] = wrapper;
    });
  }

  function buildFileTree(tree, prefix, rightPanel, registry, depth) {
    var ul = el('ul');
    var keys = Object.keys(tree).sort();
    for (var k = 0; k < keys.length; k++) {
      var name = keys[k];
      var entry = tree[name];
      var fullPath;
      if (name === '/') {
        fullPath = '/';
      } else if (prefix === '' || prefix === '/') {
        fullPath = prefix + name;
      } else {
        fullPath = prefix + '/' + name;
      }
      var hasChildren = Object.keys(entry._children).length > 0;

      (function(name, entry, fullPath, hasChildren) {
        var li = el('li');
        var node = el('div', {className: 'node'});
        if (depth > 0) node.style.paddingLeft = (depth * 16 + 6) + 'px';
        var toggle = el('span', {className: 'toggle'}, hasChildren ? '+' : ' ');
        var nameSpan = el('span', {className: 'cmd'}, name);
        var countSpan = el('span', {className: 'dur'}, '(' + entry._count + ')');

        node.appendChild(toggle);
        node.appendChild(nameSpan);
        node.appendChild(countSpan);

        var childUl = null;
        var expanded = false;

        function ensureChildren() {
          if (!childUl && hasChildren) {
            var childPrefix = (name === '/') ? '/' : fullPath;
            childUl = buildFileTree(entry._children, childPrefix, rightPanel, registry, depth + 1);
            li.appendChild(childUl);
          }
        }

        function doExpand() {
          if (!hasChildren || expanded) return;
          expanded = true;
          toggle.textContent = '-';
          ensureChildren();
          childUl.style.display = '';
        }

        function doCollapse() {
          if (!hasChildren || !expanded) return;
          expanded = false;
          toggle.textContent = '+';
          if (childUl) childUl.style.display = 'none';
        }

        if (registry) {
          registry.push({ expand: doExpand, collapse: doCollapse, hasChildren: hasChildren });
        }

        node.onclick = function(e) {
          e.stopPropagation();
          var prev = document.querySelector('.tree .node.selected');
          if (prev) prev.className = prev.className.replace(' selected', '');
          node.className += ' selected';
          showFileDetail(fullPath, rightPanel);
          if (hasChildren) {
            if (expanded) doCollapse(); else doExpand();
          }
        };

        li.appendChild(node);
        ul.appendChild(li);
      })(name, entry, fullPath, hasChildren);
    }
    return ul;
  }

  function showFileDetail(path, panel) {
    var titleEl = document.getElementById('file-detail-title');
    if (titleEl) titleEl.textContent = path;

    var old = panel.querySelector('.file-procs');
    if (old) old.remove();

    var container = el('div', {className: 'file-procs'});
    container.appendChild(el('div', {className: 'loading'}, 'Loading...'));
    panel.appendChild(container);

    api('/api/files/by-path?path=' + encodeURIComponent(path), function(data) {
      container.innerHTML = '';
      if (!data || data.length === 0) {
        var prefixUrl = '/api/files/by-prefix?prefix=' + encodeURIComponent(path);
        delete cache[prefixUrl];
        api(prefixUrl, function(d2) {
          container.innerHTML = '';
          if (!d2 || d2.length === 0) {
            container.appendChild(el('div', {style: 'color:var(--fg2)'}, 'No accesses found'));
            return;
          }
          container.appendChild(buildFileAccessTable(d2, true));
        });
        return;
      }
      container.appendChild(buildFileAccessTable(data, false));
    });
  }

  function buildFileAccessTable(data, showFilename) {
    // Compute minStart for relative times
    var minStart = 0;
    for (var i = 0; i < data.length; i++) {
      if (data[i].start_time_us != null) {
        if (minStart === 0 || data[i].start_time_us < minStart) minStart = data[i].start_time_us;
      }
    }

    // Column definitions: [label, data-col key or null (always visible)]
    // Order matches Processes tree: Duration, Start, End, Exit, CPU, RSS, I/O
    var colDefs = [
      ['PID', null],
      ['Process', null],
      ['Command', null],
      ['Mode', null],
      ['Duration', 'duration'],
      ['Start', 'start'],
      ['End', 'end'],
      ['Exit', 'exit'],
      ['CPU', 'cpu'],
      ['RSS', 'rss'],
      ['I/O', 'io']
    ];
    if (showFilename) colDefs.push(['Filename', null]);

    var tbl = el('table');

    // Header
    var thead = el('tr');
    for (var h = 0; h < colDefs.length; h++) {
      var th = el('th', null, colDefs[h][0]);
      if (colDefs[h][1]) {
        th.setAttribute('data-col', colDefs[h][1]);
      }
      thead.appendChild(th);
    }
    tbl.appendChild(el('thead', null, [thead]));

    // Body
    var tbody = el('tbody');
    for (var r = 0; r < data.length; r++) {
      var a = data[r];
      var cpu = (a.user_time_us || 0) + (a.sys_time_us || 0);
      var io = (a.io_read_bytes || 0) + (a.io_write_bytes || 0);
      var vals = [
        [String(a.pid), null],
        [cmdName(a.cmdline || ''), null],
        [a.cmdline || '', null],
        [a.mode_str, null],
        [a.duration_us != null ? formatDuration(a.duration_us) : '-', 'duration'],
        [a.start_time_us != null ? formatRelSec(a.start_time_us - minStart) : '-', 'start'],
        [a.end_time_us != null ? formatRelSec(a.end_time_us - minStart) : '-', 'end'],
        [a.exit_code != null ? String(a.exit_code) : '-', 'exit'],
        [cpu > 0 ? formatDuration(cpu) : '-', 'cpu'],
        [formatRss(a.peak_rss_kb || 0), 'rss'],
        [io > 0 ? formatBytes(io) : '-', 'io']
      ];
      if (showFilename) vals.push([a.filename, null]);

      var tr = el('tr');
      for (var c = 0; c < vals.length; c++) {
        var td = el('td', null, vals[c][0]);
        if (c === 2) {
          td.title = vals[c][0];
          td.className = 'cmd-cell';
        }
        if (vals[c][1]) {
          td.setAttribute('data-col', vals[c][1]);
        }
      tr.appendChild(td);
      }
      tbody.appendChild(tr);
    }
    tbl.appendChild(tbody);
    setTimeout(function() { enableColumnResize(tbl); }, 0);
    return tbl;
  }

  // ---- Bottlenecks ----
  function renderBottlenecks() {
    var app = document.getElementById('app');
    app.innerHTML = '<div class="loading">Loading...</div>';

    api('/api/slowest?group_by=cmd_name', function(data) {
      app.innerHTML = '';
      app.appendChild(el('div', {className: 'section-title'}, 'Bottleneck Analysis (grouped by command)'));

      var sortKey = 'total_us';
      renderGroups(data, sortKey);
    });

    function renderGroups(data, sortKey) {
      var old = document.querySelector('.bottleneck-list');
      if (old) old.remove();

      data.sort(function(a, b) { return (b[sortKey] || 0) - (a[sortKey] || 0); });

      var container = el('div', {className: 'bottleneck-list'});

      var sortRow = el('div', {style: 'margin-bottom:12px;color:var(--fg2)'}, 'Sort by: ');
      var sorts = [
        ['total_us', 'Total Time'], ['count', 'Count'], ['max_us', 'Max Time'], ['avg_us', 'Avg Time'],
        ['total_cpu_us', 'Total CPU'], ['max_cpu_us', 'Max CPU'], ['avg_cpu_us', 'Avg CPU'],
        ['max_rss_kb', 'Max RSS']
      ];
      for (var s = 0; s < sorts.length; s++) {
        (function(key, label) {
          var btn = el('span', {
            style: 'cursor:pointer;padding:4px 8px;margin-right:4px;border-radius:3px;' +
              (sortKey === key ? 'background:var(--border);color:var(--accent)' : ''),
            onclick: function() {
              sortKey = key;
              renderGroups(data, sortKey);
            }
          }, label);
          sortRow.appendChild(btn);
        })(sorts[s][0], sorts[s][1]);
      }
      container.appendChild(sortRow);

      // Compute totals for percentage
      var grandTotalUs = 0, grandTotalCpu = 0;
      for (var ti = 0; ti < data.length; ti++) {
        grandTotalUs += data[ti].total_us || 0;
        grandTotalCpu += data[ti].total_cpu_us || 0;
      }

      var tbl = document.createElement('table');
      var thead = document.createElement('thead');
      var headTr = document.createElement('tr');
      var cols = ['', 'Command', 'Count', 'Total', '%Time', 'Avg', 'Max', 'CPU Total', '%CPU', 'CPU Avg', 'CPU Max', 'Max RSS'];
      for (var ci = 0; ci < cols.length; ci++) {
        var th = document.createElement('th');
        th.textContent = cols[ci];
        if (ci === 0) th.className = 'bn-toggle';
        headTr.appendChild(th);
      }
      thead.appendChild(headTr);
      tbl.appendChild(thead);

      var tbody = document.createElement('tbody');
      for (var gi = 0; gi < data.length; gi++) {
        (function(g) {
          var tr = document.createElement('tr');
          tr.className = 'bottleneck-group';
          var toggleTd = document.createElement('td');
          toggleTd.className = 'bn-toggle';
          toggleTd.textContent = '+';
          tr.appendChild(toggleTd);

          var pctTime = grandTotalUs > 0 ? (g.total_us || 0) * 100 / grandTotalUs : 0;
          var pctCpu = grandTotalCpu > 0 ? (g.total_cpu_us || 0) * 100 / grandTotalCpu : 0;

          var vals = [
            {text: g.cmd_name},
            {text: g.count},
            {text: formatDuration(g.total_us)},
            {text: formatPct(g.total_us, grandTotalUs), pct: pctTime},
            {text: formatDuration(g.avg_us)},
            {text: formatDuration(g.max_us)},
            {text: (g.total_cpu_us || 0) > 0 ? formatDuration(g.total_cpu_us) : '-'},
            {text: formatPct(g.total_cpu_us, grandTotalCpu), pct: pctCpu},
            {text: (g.avg_cpu_us || 0) > 0 ? formatDuration(g.avg_cpu_us) : '-'},
            {text: (g.max_cpu_us || 0) > 0 ? formatDuration(g.max_cpu_us) : '-'},
            {text: formatRss(g.max_rss_kb || 0)}
          ];
          for (var vi = 0; vi < vals.length; vi++) {
            var td = document.createElement('td');
            td.textContent = vals[vi].text;
            if (vals[vi].pct != null && vals[vi].pct >= 25) {
              td.style.color = 'var(--red)';
              td.style.fontWeight = 'bold';
            } else if (vals[vi].pct != null && vals[vi].pct >= 10) {
              td.style.color = 'var(--orange)';
              td.style.fontWeight = 'bold';
            } else if (vals[vi].pct != null && vals[vi].pct >= 5) {
              td.style.color = 'var(--yellow)';
            }
            tr.appendChild(td);
          }
          tr.onclick = function() { toggleGroup(g, tr, toggleTd, tbody); };
          tbody.appendChild(tr);
        })(data[gi]);
      }
      tbl.appendChild(tbody);
      container.appendChild(tbl);
      setTimeout(function() { enableColumnResize(tbl); }, 0);

      function toggleGroup(group, groupRow, toggleTd, tbody) {
        if (groupRow._expanded) {
          // collapse: remove child rows
          var children = groupRow._children || [];
          for (var i = 0; i < children.length; i++) {
            if (children[i].parentNode) children[i].parentNode.removeChild(children[i]);
          }
          groupRow._expanded = false;
          toggleTd.textContent = '+';
          return;
        }
        // expand
        if (groupRow._children) {
          insertChildren(groupRow, groupRow._children, tbody);
          groupRow._expanded = true;
          toggleTd.textContent = '\u2212';
          return;
        }
        toggleTd.textContent = '...';
        api('/api/slowest?cmd=' + encodeURIComponent(group.cmd_name), function(procs) {
          var rows = [];
          for (var i = 0; i < procs.length; i++) {
            var p = procs[i];
            var cr = document.createElement('tr');
            cr.className = 'bottleneck-child';
            // toggle placeholder
            var ct0 = document.createElement('td');
            ct0.className = 'bn-toggle';
            cr.appendChild(ct0);
            // cmdline with pid
            var ct1 = document.createElement('td');
            ct1.className = 'cmd-cell';
            ct1.textContent = '[' + p.pid + '] ' + p.cmdline;
            ct1.title = p.cmdline;
            cr.appendChild(ct1);
            // exit_code (Count col)
            var ct2 = document.createElement('td');
            ct2.textContent = p.exit_code != null ? p.exit_code : '-';
            if (p.exit_code && p.exit_code !== 0) ct2.style.color = 'var(--red)';
            cr.appendChild(ct2);
            // duration (Total col)
            var ct3 = document.createElement('td');
            ct3.textContent = formatDuration(p.duration_us);
            cr.appendChild(ct3);
            // %Time
            var childPctTime = grandTotalUs > 0 ? (p.duration_us || 0) * 100 / grandTotalUs : 0;
            var ctPctTime = document.createElement('td');
            ctPctTime.textContent = formatPct(p.duration_us, grandTotalUs);
            if (childPctTime >= 25) { ctPctTime.style.color = 'var(--red)'; ctPctTime.style.fontWeight = 'bold'; }
            else if (childPctTime >= 10) { ctPctTime.style.color = 'var(--orange)'; ctPctTime.style.fontWeight = 'bold'; }
            else if (childPctTime >= 5) { ctPctTime.style.color = 'var(--yellow)'; }
            cr.appendChild(ctPctTime);
            // avg, max placeholders
            cr.appendChild(document.createElement('td')).textContent = '-';
            cr.appendChild(document.createElement('td')).textContent = '-';
            // cpu (CPU Total col)
            var cpuVal = (p.user_time_us || 0) + (p.sys_time_us || 0);
            var ct6 = document.createElement('td');
            ct6.textContent = cpuVal > 0 ? formatDuration(cpuVal) : '-';
            cr.appendChild(ct6);
            // %CPU
            var childPctCpu = grandTotalCpu > 0 ? cpuVal * 100 / grandTotalCpu : 0;
            var ctPctCpu = document.createElement('td');
            ctPctCpu.textContent = formatPct(cpuVal, grandTotalCpu);
            if (childPctCpu >= 25) { ctPctCpu.style.color = 'var(--red)'; ctPctCpu.style.fontWeight = 'bold'; }
            else if (childPctCpu >= 10) { ctPctCpu.style.color = 'var(--orange)'; ctPctCpu.style.fontWeight = 'bold'; }
            else if (childPctCpu >= 5) { ctPctCpu.style.color = 'var(--yellow)'; }
            cr.appendChild(ctPctCpu);
            // cpu avg, cpu max placeholders
            cr.appendChild(document.createElement('td')).textContent = '-';
            cr.appendChild(document.createElement('td')).textContent = '-';
            // rss
            var ct9 = document.createElement('td');
            ct9.textContent = formatRss(p.peak_rss_kb || 0);
            cr.appendChild(ct9);
            rows.push(cr);
          }
          groupRow._children = rows;
          insertChildren(groupRow, rows, tbody);
          groupRow._expanded = true;
          toggleTd.textContent = '\u2212';
        });
      }

      function insertChildren(groupRow, children, tbody) {
        var next = groupRow.nextSibling;
        for (var i = 0; i < children.length; i++) {
          tbody.insertBefore(children[i], next);
        }
      }

      var app = document.getElementById('app');
      app.appendChild(container);
    }
  }

  // ---- Timeline ----
  function renderTimeline() {
    var app = document.getElementById('app');
    app.innerHTML = '<div class="loading">Loading timeline data...</div>';

    api('/api/timeline?min_duration_us=0', function(data) {
      app.innerHTML = '';
      var container = el('div', {className: 'timeline-container'});
      var canvas = el('canvas');
      var tooltip = el('div', {className: 'timeline-tooltip'});
      container.appendChild(canvas);
      container.appendChild(tooltip);
      app.appendChild(container);

      if (data.processes && data.processes.length > 0) {
        TimelineRenderer.init(canvas, tooltip, data);
      } else {
        app.innerHTML = '<div class="loading">No timeline data available</div>';
      }
    });
  }

  // ---- Analysis ----
  function renderAnalysis() {
    var app = document.getElementById('app');
    app.innerHTML = '';

    var tabs = ['Critical Path', 'Hotspots', 'Failures', 'Diagnostics', 'Impact', 'Races', 'Rebuild', 'Rdeps'];
    var activeTab = 0;

    var tabRow = el('div', {className: 'analysis-tabs'});
    var content = el('div', {className: 'analysis-content'});

    for (var t = 0; t < tabs.length; t++) {
      (function(idx) {
        var tab = el('div', {
          className: 'analysis-tab' + (idx === 0 ? ' active' : ''),
          onclick: function() {
            activeTab = idx;
            var allTabs = tabRow.querySelectorAll('.analysis-tab');
            for (var i = 0; i < allTabs.length; i++) allTabs[i].className = 'analysis-tab' + (i === idx ? ' active' : '');
            loadAnalysisTab(idx, content);
          }
        }, tabs[idx]);
        tabRow.appendChild(tab);
      })(t);
    }

    app.appendChild(tabRow);
    app.appendChild(content);
    loadAnalysisTab(0, content);
  }

  function loadAnalysisTab(idx, content) {
    content.innerHTML = '<div class="loading">Loading...</div>';

    if (idx === 0) {
      api('/api/critical-path', function(data) {
        content.innerHTML = '';
        content.appendChild(el('div', {className: 'section-title'}, 'Critical Path'));
        if (!data.path || data.path.length === 0) {
          content.appendChild(el('div', null, 'No critical path data'));
          return;
        }
        content.appendChild(el('div', {style: 'margin-bottom:12px;color:var(--fg2)'}, 'Total: ' + formatDuration(data.total_us)));
        for (var i = 0; i < data.path.length; i++) {
          var p = data.path[i];
          var indent = i * 20;
          var cmdText = shortenCmd(p.cmdline, 60);
          var row = el('div', {style: 'padding:4px 8px;padding-left:' + (indent + 8) + 'px;border-left:2px solid var(--accent);margin-bottom:2px;background:var(--bg3);border-radius:0 4px 4px 0'}, [
            el('span', {style: 'color:var(--fg2)'}, '[' + p.pid + '] '),
            spanWithTitle(cmdText, p.cmdline),
            el('span', {style: 'color:var(--accent);margin-left:12px'}, formatDuration(p.duration_us))
          ]);
          content.appendChild(row);
        }
      });
    } else if (idx === 1) {
      api('/api/hotspots?top=30&by_dir=0', function(data) {
        content.innerHTML = '';
        content.appendChild(el('div', {className: 'section-title'}, 'File Access Hotspots'));
        if (!data || data.length === 0) {
          content.appendChild(el('div', null, 'No hotspot data'));
          return;
        }
        var tbl = makeTable(['Count', 'Reads', 'Writes', 'Procs', 'Path'], data.map(function(h) {
          return [h.total, h.reads, h.writes, h.num_procs, h.name];
        }));
        content.appendChild(tbl);
      });
    } else if (idx === 2) {
      api('/api/failures', function(data) {
        content.innerHTML = '';
        content.appendChild(el('div', {className: 'section-title'}, 'Failed File Accesses'));
        if (!data.total || data.total === 0) {
          content.appendChild(el('div', null, 'No failed accesses'));
          return;
        }
        content.appendChild(el('div', {style: 'margin-bottom:12px;color:var(--fg2)'}, 'Total: ' + data.total));

        if (data.by_errno && data.by_errno.length > 0) {
          content.appendChild(el('div', {style: 'margin:12px 0 8px;font-weight:bold'}, 'By Error Code'));
          content.appendChild(makeTable(['Errno', 'Name', 'Count'], data.by_errno.map(function(e) {
            return [e.errno_val, e.name, e.count];
          })));
        }

        if (data.by_file && data.by_file.length > 0) {
          content.appendChild(el('div', {style: 'margin:12px 0 8px;font-weight:bold'}, 'Top Files'));
          content.appendChild(makeTable(['Count', 'File'], data.by_file.map(function(f) {
            return [f.count, f.filename];
          })));
        }
      });
    } else if (idx === 3) {
      api('/api/diagnostics', function(data) {
        content.innerHTML = '';
        content.appendChild(el('div', {className: 'section-title'}, 'Auto Diagnostics'));
        if (!data.issues || data.issues.length === 0) {
          content.appendChild(el('div', {style: 'color:var(--green)'}, 'No significant issues detected.'));
          return;
        }
        for (var i = 0; i < data.issues.length; i++) {
          var issue = data.issues[i];
          var card = el('div', {className: 'diag-card ' + issue.type.toLowerCase()});
          card.appendChild(el('div', {className: 'diag-type'}, issue.type));
          card.appendChild(el('div', null, issue.message));
          if (issue.detail) card.appendChild(el('div', {style: 'color:var(--fg2);margin-top:4px'}, issue.detail));
          content.appendChild(card);
        }
      });
    } else if (idx === 4) {
      api('/api/impact?top=30', function(data) {
        content.innerHTML = '';
        content.appendChild(el('div', {className: 'section-title'}, 'Impact Ranking'));
        if (!data || data.length === 0) {
          content.appendChild(el('div', null, 'No impact data'));
          return;
        }
        content.appendChild(el('div', {style: 'margin-bottom:12px;color:var(--fg2)'}, 'Source files ranked by rebuild impact (affected processes \u00d7 duration)'));
        var tbl = makeTable(['Procs', 'Duration', 'File'], data.map(function(e) {
          return [e.affected_procs, formatDuration(e.affected_duration_us), e.file];
        }));
        content.appendChild(tbl);
      });
    } else if (idx === 5) {
      api('/api/races', function(data) {
        content.innerHTML = '';
        content.appendChild(el('div', {className: 'section-title'}, 'Race Condition Detection'));
        if (!data.races || data.races.length === 0) {
          content.appendChild(el('div', {style: 'color:var(--green)'}, 'No race conditions detected.'));
          return;
        }
        content.appendChild(el('div', {style: 'margin-bottom:12px;color:var(--fg2)'}, data.count + ' potential race(s) found'));
        var tbl = makeTable(['File', 'Writer', 'Reader', 'Overlap'], data.races.map(function(r) {
          return [r.file, '[' + r.writer_pid + '] ' + shortenCmd(r.writer_cmd, 40), '[' + r.reader_pid + '] ' + shortenCmd(r.reader_cmd, 40), formatDuration(r.overlap_us)];
        }));
        content.appendChild(tbl);
      });
    } else if (idx === 6) {
      content.innerHTML = '';
      content.appendChild(el('div', {className: 'section-title'}, 'Rebuild Estimator'));

      // File search input with autocomplete suggestions
      var searchRow = el('div', {style: 'display:flex;gap:8px;margin-bottom:4px;align-items:center'});
      var searchWrap = el('div', {style: 'flex:1;position:relative'});
      var input = el('input', {type: 'text', placeholder: 'Type to search files...', style: 'width:100%;box-sizing:border-box;padding:6px 10px;background:var(--bg3);border:1px solid var(--border);color:var(--fg);border-radius:4px'});
      var suggestBox = el('div', {style: 'display:none;position:absolute;left:0;right:0;top:100%;max-height:200px;overflow-y:auto;background:var(--bg2);border:1px solid var(--border);border-radius:0 0 4px 4px;z-index:100'});
      searchWrap.appendChild(input);
      searchWrap.appendChild(suggestBox);
      var btn = el('button', {style: 'padding:6px 16px;background:var(--accent);color:#fff;border:none;border-radius:4px;cursor:pointer'}, 'Calculate');
      searchRow.appendChild(searchWrap);
      searchRow.appendChild(btn);
      content.appendChild(searchRow);

      // Selected files list
      var selectedDiv = el('div', {style: 'margin-bottom:12px;display:flex;flex-wrap:wrap;gap:4px'});
      content.appendChild(selectedDiv);
      var selectedFiles = [];

      function renderSelected() {
        selectedDiv.innerHTML = '';
        for (var si = 0; si < selectedFiles.length; si++) {
          (function(idx) {
            var tag = el('span', {style: 'display:inline-flex;align-items:center;gap:4px;padding:2px 8px;background:var(--bg3);border:1px solid var(--border);border-radius:3px;font-size:12px;max-width:100%;word-break:break-all'});
            tag.appendChild(document.createTextNode(selectedFiles[idx]));
            var rm = el('span', {style: 'cursor:pointer;color:var(--fg2);font-weight:bold'}, '\u00d7');
            rm.onclick = function() { selectedFiles.splice(idx, 1); renderSelected(); };
            tag.appendChild(rm);
            selectedDiv.appendChild(tag);
          })(si);
        }
      }

      // Fetch file list for autocomplete
      var allFilePaths = [];
      api('/api/files', function(data) {
        if (data) {
          for (var fi = 0; fi < data.length; fi++) {
            allFilePaths.push(data[fi].path);
          }
        }
      });

      var suggestIndex = -1;
      var suggestMatches = [];

      function selectSuggestion(path) {
        if (selectedFiles.indexOf(path) < 0) {
          selectedFiles.push(path);
          renderSelected();
        }
        input.value = '';
        suggestBox.style.display = 'none';
        suggestIndex = -1;
      }

      function updateSuggestHighlight() {
        var items = suggestBox.children;
        for (var i = 0; i < items.length; i++) {
          items[i].style.background = i === suggestIndex ? 'var(--bg3)' : '';
        }
        if (suggestIndex >= 0 && suggestIndex < items.length) {
          items[suggestIndex].scrollIntoView({block: 'nearest'});
        }
      }

      function showSuggestions(query) {
        suggestBox.innerHTML = '';
        suggestIndex = -1;
        if (!query) { suggestBox.style.display = 'none'; return; }
        var q = query.toLowerCase();
        suggestMatches = [];
        for (var fi = 0; fi < allFilePaths.length; fi++) {
          if (allFilePaths[fi].toLowerCase().indexOf(q) >= 0) {
            suggestMatches.push(allFilePaths[fi]);
            if (suggestMatches.length >= 20) break;
          }
        }
        if (suggestMatches.length === 0) { suggestBox.style.display = 'none'; return; }
        for (var mi = 0; mi < suggestMatches.length; mi++) {
          (function(path, idx) {
            var item = el('div', {style: 'padding:4px 8px;cursor:pointer;font-size:12px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis'}, path);
            item.title = path;
            item.onmouseenter = function() { suggestIndex = idx; updateSuggestHighlight(); };
            item.onclick = function() { selectSuggestion(path); };
            suggestBox.appendChild(item);
          })(suggestMatches[mi], mi);
        }
        suggestBox.style.display = 'block';
      }

      input.oninput = function() { showSuggestions(input.value.trim()); };
      input.onkeydown = function(e) {
        if (suggestBox.style.display !== 'none' && suggestMatches.length > 0) {
          if (e.key === 'ArrowDown') {
            e.preventDefault();
            suggestIndex = suggestIndex < suggestMatches.length - 1 ? suggestIndex + 1 : 0;
            updateSuggestHighlight();
            return;
          } else if (e.key === 'ArrowUp') {
            e.preventDefault();
            suggestIndex = suggestIndex > 0 ? suggestIndex - 1 : suggestMatches.length - 1;
            updateSuggestHighlight();
            return;
          } else if (e.key === 'Enter' && suggestIndex >= 0) {
            e.preventDefault();
            selectSuggestion(suggestMatches[suggestIndex]);
            return;
          }
        }
        if (e.key === 'Enter') {
          var val = input.value.trim();
          if (val && selectedFiles.indexOf(val) < 0) {
            selectedFiles.push(val);
            renderSelected();
          }
          input.value = '';
          suggestBox.style.display = 'none';
        }
      };
      document.addEventListener('click', function(e) {
        if (!searchWrap.contains(e.target)) { suggestBox.style.display = 'none'; suggestIndex = -1; }
      });

      // Collapse option
      var collapseRow = el('div', {style: 'display:flex;gap:8px;margin-bottom:12px;align-items:center'});
      collapseRow.appendChild(el('span', {style: 'color:var(--fg2);font-size:12px;white-space:nowrap'}, 'Collapse:'));
      var collapseInput = el('input', {type: 'text', placeholder: 'e.g. make,sh,bash', style: 'flex:1;padding:6px 10px;background:var(--bg3);border:1px solid var(--border);color:var(--fg);border-radius:4px;font-size:12px'});
      collapseRow.appendChild(collapseInput);
      content.appendChild(collapseRow);

      var resultDiv = el('div');
      content.appendChild(resultDiv);
      btn.onclick = function() {
        var val = selectedFiles.length > 0 ? selectedFiles.join(',') : input.value.trim();
        if (!val) return;
        resultDiv.innerHTML = '<div class="loading">Calculating...</div>';
        var collapseVal = collapseInput.value.split(',').map(function(s) { return s.trim(); }).filter(function(s) { return s; }).join(',');
        var url = '/api/rebuild?changed=' + encodeURIComponent(val) + '&estimate=1';
        if (collapseVal) url += '&collapse=' + encodeURIComponent(collapseVal);
        api(url, function(data) {
          resultDiv.innerHTML = '';
          if (!data || data.affected_count === 0) {
            resultDiv.appendChild(el('div', null, 'No affected processes for given files.'));
            return;
          }
          resultDiv.appendChild(el('div', {style: 'margin-bottom:8px'}, 'Affected processes: ' + data.affected_count));
          resultDiv.appendChild(el('div', {style: 'margin-bottom:8px'}, 'Serial estimate: ' + formatDuration(data.serial_estimate_us)));
          resultDiv.appendChild(el('div', {style: 'margin-bottom:12px'}, 'Longest single: ' + formatDuration(data.longest_single_us)));
          if (data.affected && data.affected.length > 0) {
            var minStart = data.trace_min_start_us || 0;
            var tbl = document.createElement('table');
            tbl.className = 'data-table';
            var thead = document.createElement('thead');
            var hr = document.createElement('tr');
            var cols = ['', 'PID', 'Start', 'Duration', 'Directory', 'Command'];
            for (var ci = 0; ci < cols.length; ci++) {
              var th = document.createElement('th');
              th.textContent = cols[ci];
              hr.appendChild(th);
            }
            thead.appendChild(hr);
            tbl.appendChild(thead);
            var tbody = document.createElement('tbody');
            for (var ri = 0; ri < data.affected.length; ri++) {
              (function(p) {
                var tr = document.createElement('tr');
                var toggleTd = document.createElement('td');
                toggleTd.style.cssText = 'cursor:pointer;user-select:none;width:20px;text-align:center';
                toggleTd.textContent = '+';
                tr.appendChild(toggleTd);
                var vals = [p.pid, formatRelSec(p.start_time_us - minStart), formatDuration(p.duration_us), p.cwd || '', p.cmdline];
                for (var vi = 0; vi < vals.length; vi++) {
                  var td = document.createElement('td');
                  td.textContent = vals[vi];
                  tr.appendChild(td);
                }
                tbody.appendChild(tr);
                // Reason detail row (hidden by default)
                var reasonTr = document.createElement('tr');
                reasonTr.style.display = 'none';
                var reasonTd = document.createElement('td');
                reasonTd.colSpan = cols.length;
                reasonTd.style.cssText = 'font-size:12px;color:var(--fg2);background:var(--bg1);padding-left:32px';
                var hasContent = false;
                if (p.reads && p.reads.length > 0) {
                  for (var rj = 0; rj < p.reads.length; rj++) {
                    var line = document.createElement('div');
                    line.textContent = 'Read  ' + p.reads[rj];
                    reasonTd.appendChild(line);
                    hasContent = true;
                  }
                }
                if (p.writes && p.writes.length > 0) {
                  for (var wj = 0; wj < p.writes.length; wj++) {
                    var line = document.createElement('div');
                    line.textContent = 'Write ' + p.writes[wj];
                    reasonTd.appendChild(line);
                    hasContent = true;
                  }
                }
                if (!hasContent) {
                  reasonTd.textContent = 'No chain details available';
                }
                reasonTr.appendChild(reasonTd);
                tbody.appendChild(reasonTr);
                toggleTd.onclick = function() {
                  if (reasonTr.style.display === 'none') {
                    reasonTr.style.display = '';
                    toggleTd.textContent = '\u2212';
                  } else {
                    reasonTr.style.display = 'none';
                    toggleTd.textContent = '+';
                  }
                };
              })(data.affected[ri]);
            }
            tbl.appendChild(tbody);
            resultDiv.appendChild(tbl);

            // Copyable rebuild script
            var lines = [];
            var lastCwd = null;
            for (var pi = 0; pi < data.affected.length; pi++) {
              var p = data.affected[pi];
              if (pi > 0) lines.push('');
              if (p.cwd && p.cwd !== lastCwd) {
                lines.push('cd ' + p.cwd);
                lines.push('');
                lastCwd = p.cwd;
              }
              lines.push(p.cmdline);
            }
            var script = lines.join('\n');
            var scriptDiv = el('div', {style: 'margin-top:12px'});
            var copyBtn = el('button', {style: 'padding:4px 12px;margin-bottom:8px;background:var(--bg3);border:1px solid var(--border);color:var(--fg);border-radius:4px;cursor:pointer'}, 'Copy rebuild script');
            copyBtn.onclick = function() {
              if (navigator.clipboard) {
                navigator.clipboard.writeText(script);
                copyBtn.textContent = 'Copied!';
                setTimeout(function() { copyBtn.textContent = 'Copy rebuild script'; }, 2000);
              }
            };
            scriptDiv.appendChild(copyBtn);
            var pre = el('pre', {style: 'padding:8px 12px;background:var(--bg1);border:1px solid var(--border);border-radius:4px;overflow-x:auto;font-size:12px;white-space:pre-wrap;word-break:break-all'});
            pre.textContent = script;
            scriptDiv.appendChild(pre);
            resultDiv.appendChild(scriptDiv);
          }
        });
      };
    } else if (idx === 7) {
      content.innerHTML = '';
      content.appendChild(el('div', {className: 'section-title'}, 'Reverse Dependencies'));
      var row = el('div', {style: 'display:flex;gap:8px;margin-bottom:12px;align-items:center'});
      var input = el('input', {type: 'text', placeholder: 'Enter file path', style: 'flex:1;padding:6px 10px;background:var(--bg3);border:1px solid var(--border);color:var(--fg);border-radius:4px'});
      var depthInput = el('input', {type: 'number', value: '3', min: '1', max: '10', style: 'width:60px;padding:6px 10px;background:var(--bg3);border:1px solid var(--border);color:var(--fg);border-radius:4px'});
      var btn = el('button', {style: 'padding:6px 16px;background:var(--accent);color:#fff;border:none;border-radius:4px;cursor:pointer'}, 'Search');
      row.appendChild(input);
      row.appendChild(el('span', {style: 'color:var(--fg2)'}, 'Depth:'));
      row.appendChild(depthInput);
      row.appendChild(btn);
      content.appendChild(row);
      var resultDiv = el('div');
      content.appendChild(resultDiv);
      btn.onclick = function() {
        var val = input.value.trim();
        if (!val) return;
        var depth = parseInt(depthInput.value) || 3;
        resultDiv.innerHTML = '<div class="loading">Searching...</div>';
        api('/api/rdeps?file=' + encodeURIComponent(val) + '&depth=' + depth, function(data) {
          resultDiv.innerHTML = '';
          if (!data || !data.file) {
            resultDiv.appendChild(el('div', null, 'File not found in trace.'));
            return;
          }
          function renderNode(node, indent) {
            var d = el('div', {style: 'padding:2px 0;padding-left:' + (indent * 20) + 'px'});
            var arrow = indent > 0 ? '\u2192 ' : '';
            var via = node.via_pid ? ' (via [' + node.via_pid + '] ' + shortenCmd(node.via_cmd || '', 30) + ')' : '';
            d.appendChild(el('span', {style: 'color:var(--accent)'}, arrow + node.file));
            if (via) d.appendChild(el('span', {style: 'color:var(--fg2)'}, via));
            resultDiv.appendChild(d);
            if (node.children) {
              for (var i = 0; i < node.children.length; i++) renderNode(node.children[i], indent + 1);
            }
          }
          renderNode(data, 0);
        });
      };
    }
  }

  // ---- Navigation ----
  function switchView(view) {
    currentView = view;
    var tabs = document.querySelectorAll('#navbar .tab');
    for (var i = 0; i < tabs.length; i++) {
      tabs[i].className = 'tab' + (tabs[i].getAttribute('data-view') === view ? ' active' : '');
    }

    var app = document.getElementById('app');
    while (app.firstChild) app.removeChild(app.firstChild);

    if ((view === 'processes' || view === 'files') && viewDom[view]) {
      app.appendChild(viewDom[view]);
      return;
    }

    switch (view) {
      case 'summary': renderSummary(); break;
      case 'processes': renderProcesses(); break;
      case 'files': renderFiles(); break;
      case 'bottlenecks': renderBottlenecks(); break;
      case 'timeline': renderTimeline(); break;
      case 'analysis': renderAnalysis(); break;
    }
  }

  document.addEventListener('DOMContentLoaded', function() {
    var tabs = document.querySelectorAll('#navbar .tab');
    for (var i = 0; i < tabs.length; i++) {
      tabs[i].addEventListener('click', function(e) {
        e.preventDefault();
        switchView(this.getAttribute('data-view'));
      });
    }
    syncColumnClasses();
    switchView('summary');
  });

  return { switchView: switchView };
})();
