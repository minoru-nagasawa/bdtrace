// bdtrace Web UI - app.js

var App = (function() {
  var currentView = 'summary';
  var cache = {};
  var viewDom = {};  // cached rendered DOM per view

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

  function shortenCmd(cmd, max) {
    if (!max || cmd.length <= max) return cmd;
    return cmd.substr(0, max - 3) + '...';
  }

  function cmdName(cmdline) {
    var first = cmdline.split(' ')[0];
    var sl = first.lastIndexOf('/');
    return sl >= 0 ? first.substr(sl + 1) : first;
  }

  // ---- Tree control helpers ----
  // registry: array of { expand:fn, collapse:fn, hasChildren:bool }
  function expandAll(registry) {
    // Repeatedly expand until no new nodes appear (lazy-built children register themselves)
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

  function makeTreeToolbar(registry) {
    var bar = el('div', {style: 'display:flex;gap:8px;margin-bottom:8px'});
    bar.appendChild(el('span', {
      className: 'tree-btn',
      onclick: function() { expandAll(registry); }
    }, 'Expand All'));
    bar.appendChild(el('span', {
      className: 'tree-btn',
      onclick: function() { collapseAll(registry); }
    }, 'Collapse All'));
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

      // Top 5 slowest
      if (s.slowest && s.slowest.length > 0) {
        app.appendChild(el('div', {className: 'section-title'}, 'Slowest Processes'));
        var tbl = makeTable(['PID', 'Duration', 'Command'], s.slowest.map(function(p) {
          return [p.pid, formatDuration(p.duration_us), shortenCmd(p.cmdline, 80)];
        }));
        app.appendChild(tbl);
      }

      // Parallelism chart
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

      viewDom['summary'] = app.innerHTML ? null : null; // summary doesn't need caching
    });
  }

  function kpiCard(label, value) {
    return el('div', {className: 'kpi'}, [
      el('div', {className: 'label'}, label),
      el('div', {className: 'value'}, String(value))
    ]);
  }

  function makeTable(headers, rows, sortable) {
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

    // Axes
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
  function renderProcesses() {
    // Check for cached DOM
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
      var split = el('div', {className: 'split'});
      var left = el('div', {className: 'left'});
      var right = el('div', {className: 'right'});

      left.appendChild(el('div', {className: 'section-title'}, 'Process Tree'));
      right.appendChild(el('div', {className: 'section-title', id: 'proc-detail-title'}, 'Select a process'));

      var totalDur = 0;
      var minStart = 0;
      var procs = data.processes;
      var procMap = {};
      for (var i = 0; i < procs.length; i++) {
        procMap[procs[i].pid] = procs[i];
        var d = procs[i].end_time_us - procs[i].start_time_us;
        if (d > totalDur) totalDur = d;
        if (i === 0 || procs[i].start_time_us < minStart) minStart = procs[i].start_time_us;
      }

      var nodeRegistry = [];

      // Toolbar
      left.appendChild(makeTreeToolbar(nodeRegistry));

      // Tree header
      var hdr = el('div', {className: 'tree-header'});
      hdr.appendChild(el('span', {className: 'th-tree'}, 'Process'));
      hdr.appendChild(el('span', {className: 'th-col'}, 'Files'));
      hdr.appendChild(el('span', {className: 'th-col th-dur'}, 'Duration'));
      hdr.appendChild(el('span', {className: 'th-col'}, 'Start'));
      hdr.appendChild(el('span', {className: 'th-col'}, 'End'));
      hdr.appendChild(el('span', {className: 'th-col'}, 'CPU'));
      hdr.appendChild(el('span', {className: 'th-col'}, 'RSS'));
      hdr.appendChild(el('span', {className: 'th-col'}, 'I/O'));
      hdr.appendChild(el('span', {className: 'th-col th-exit'}, 'Exit'));
      left.appendChild(hdr);

      var treeDiv = el('div', {className: 'tree'});
      var roots = data.roots || [];
      var rootUl = el('ul');
      for (var r = 0; r < roots.length; r++) {
        rootUl.appendChild(buildTreeNode(roots[r], procMap, data.children_map, totalDur, minStart, right, nodeRegistry));
      }
      treeDiv.appendChild(rootUl);
      left.appendChild(treeDiv);

      split.appendChild(left);
      split.appendChild(right);
      wrapper.appendChild(split);
      app.appendChild(wrapper);
      viewDom['processes'] = wrapper;
    });
  }

  function buildTreeNode(pid, procMap, childMap, totalDur, minStart, rightPanel, registry) {
    var proc = procMap[pid];
    if (!proc) return el('li');
    var dur = proc.end_time_us - proc.start_time_us;
    var children = childMap[pid] || [];
    var hasChildren = children.length > 0;
    var expanded = false;

    var li = el('li');
    var node = el('div', {className: 'node'});
    var toggle = el('span', {className: 'toggle'}, hasChildren ? '+' : ' ');
    var pidSpan = el('span', {className: 'pid'}, '[' + pid + ']');
    var cmdSpan = el('span', {className: 'cmd'}, cmdName(proc.cmdline));
    if (proc.exit_code !== 0) cmdSpan.className += ' exit-err';

    var filesSpan = el('span', {className: 'col-num'}, proc.file_count != null ? String(proc.file_count) : '-');
    var durSpan = el('span', {className: 'col-num col-dur' + (dur > totalDur * 0.25 ? ' slow' : '')}, formatDuration(dur));
    var startSpan = el('span', {className: 'col-num'}, formatRelSec(proc.start_time_us - minStart));
    var endSpan = el('span', {className: 'col-num'}, formatRelSec(proc.end_time_us - minStart));
    var cpuTotal = (proc.user_time_us || 0) + (proc.sys_time_us || 0);
    var cpuSpan = el('span', {className: 'col-num'}, cpuTotal > 0 ? formatDuration(cpuTotal) : '-');
    var rssSpan = el('span', {className: 'col-num'}, formatRss(proc.peak_rss_kb || 0));
    var ioTotal = (proc.io_read_bytes || 0) + (proc.io_write_bytes || 0);
    var ioSpan = el('span', {className: 'col-num'}, ioTotal > 0 ? formatBytes(ioTotal) : '-');
    var exitSpan = el('span', {className: 'col-num col-exit' + (proc.exit_code !== 0 ? ' exit-err' : '')}, String(proc.exit_code));

    node.appendChild(toggle);
    node.appendChild(pidSpan);
    node.appendChild(cmdSpan);
    node.appendChild(filesSpan);
    node.appendChild(durSpan);
    node.appendChild(startSpan);
    node.appendChild(endSpan);
    node.appendChild(cpuSpan);
    node.appendChild(rssSpan);
    node.appendChild(ioSpan);
    node.appendChild(exitSpan);

    var childUl = null;

    function ensureChildren() {
      if (!childUl && hasChildren) {
        childUl = el('ul');
        for (var c = 0; c < children.length; c++) {
          childUl.appendChild(buildTreeNode(children[c], procMap, childMap, totalDur, minStart, rightPanel, registry));
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
      // Select
      var prev = document.querySelector('.tree .node.selected');
      if (prev) prev.className = prev.className.replace(' selected', '');
      node.className += ' selected';
      showProcessDetail(pid, proc, rightPanel);

      // Toggle expand
      if (hasChildren) {
        if (expanded) doCollapse(); else doExpand();
      }
    };

    li.appendChild(node);
    return li;
  }

  function showProcessDetail(pid, proc, panel) {
    var titleEl = document.getElementById('proc-detail-title');
    if (titleEl) titleEl.textContent = '[' + pid + '] ' + shortenCmd(proc.cmdline, 60);

    // Remove old detail
    var old = panel.querySelector('.proc-files');
    if (old) old.remove();

    var container = el('div', {className: 'proc-files'});
    container.appendChild(el('div', {style: 'margin-bottom:8px;color:var(--fg2)'}, [
      el('span', null, 'Exit: '),
      el('span', {className: proc.exit_code === 0 ? '' : 'exit-err'}, String(proc.exit_code)),
      el('span', null, '  Duration: ' + formatDuration(proc.end_time_us - proc.start_time_us))
    ]));
    var cpuUser = proc.user_time_us || 0, cpuSys = proc.sys_time_us || 0;
    if (cpuUser > 0 || cpuSys > 0 || (proc.peak_rss_kb || 0) > 0) {
      container.appendChild(el('div', {style: 'margin-bottom:4px;color:var(--fg2)'}, [
        el('span', null, 'CPU: ' + formatDuration(cpuUser) + ' user, ' + formatDuration(cpuSys) + ' sys'),
        el('span', null, '  RSS: ' + formatRss(proc.peak_rss_kb || 0)),
        el('span', null, '  I/O: R:' + formatBytes(proc.io_read_bytes || 0) + ' W:' + formatBytes(proc.io_write_bytes || 0))
      ]));
    }
    container.appendChild(el('div', {style: 'margin-bottom:4px;color:var(--fg2);word-break:break-all'}, proc.cmdline));
    container.appendChild(el('div', {className: 'loading'}, 'Loading files...'));
    panel.appendChild(container);

    api('/api/processes/' + pid + '/files', function(files) {
      container.innerHTML = '';
      container.appendChild(el('div', {style: 'margin-bottom:8px;color:var(--fg2)'}, [
        el('span', null, 'Exit: '),
        el('span', {className: proc.exit_code === 0 ? '' : 'exit-err'}, String(proc.exit_code)),
        el('span', null, '  Duration: ' + formatDuration(proc.end_time_us - proc.start_time_us))
      ]));
      var cpuUser2 = proc.user_time_us || 0, cpuSys2 = proc.sys_time_us || 0;
      if (cpuUser2 > 0 || cpuSys2 > 0 || (proc.peak_rss_kb || 0) > 0) {
        container.appendChild(el('div', {style: 'margin-bottom:4px;color:var(--fg2)'}, [
          el('span', null, 'CPU: ' + formatDuration(cpuUser2) + ' user, ' + formatDuration(cpuSys2) + ' sys'),
          el('span', null, '  RSS: ' + formatRss(proc.peak_rss_kb || 0)),
          el('span', null, '  I/O: R:' + formatBytes(proc.io_read_bytes || 0) + ' W:' + formatBytes(proc.io_write_bytes || 0))
        ]));
      }
      container.appendChild(el('div', {style: 'margin-bottom:8px;color:var(--fg2);word-break:break-all'}, proc.cmdline));

      if (files.length === 0) {
        container.appendChild(el('div', {style: 'color:var(--fg2)'}, 'No file accesses'));
        return;
      }

      // Aggregate files by filename
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

      // Build tree structure from aggregated files
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

      // Combine into single tree with / root for absolute paths
      var tree = {};
      if (Object.keys(absTree).length > 0) {
        tree['/'] = {_info: null, _children: absTree};
      }
      var relKeys = Object.keys(relTree);
      for (var i = 0; i < relKeys.length; i++) {
        tree[relKeys[i]] = relTree[relKeys[i]];
      }

      // Header for proc file tree
      var fileHdr = el('div', {className: 'tree-header'});
      fileHdr.appendChild(el('span', {className: 'th-tree'}, 'File'));
      fileHdr.appendChild(el('span', {className: 'th-col'}, 'Mode'));
      fileHdr.appendChild(el('span', {className: 'th-col th-exit'}, 'Count'));
      container.appendChild(fileHdr);

      var treeDiv = el('div', {className: 'tree'});
      treeDiv.appendChild(buildProcFileTree(tree));
      container.appendChild(treeDiv);

      container.appendChild(el('div', {style: 'margin-top:8px;color:var(--fg2);font-size:11px'}, filenames.length + ' unique files, ' + files.length + ' total accesses'));
    });
  }

  function buildProcFileTree(tree) {
    var ul = el('ul');
    var keys = Object.keys(tree).sort();
    for (var k = 0; k < keys.length; k++) {
      (function(name) {
        var entry = tree[name];
        var hasChildren = Object.keys(entry._children).length > 0;
        var info = entry._info;

        var li = el('li');
        var node = el('div', {className: 'node'});
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

        node.onclick = function(e) {
          e.stopPropagation();
          if (hasChildren) {
            expanded = !expanded;
            toggle.textContent = expanded ? '-' : '+';
            if (expanded && !childUl) {
              childUl = buildProcFileTree(entry._children);
              li.appendChild(childUl);
            }
            if (childUl) childUl.style.display = expanded ? '' : 'none';
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
    // Check for cached DOM
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

      // Build directory tree, separating absolute and relative paths
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

      // Combine: absolute paths under '/' root, relative paths at top level
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

      // Toolbar
      left.appendChild(makeTreeToolbar(nodeRegistry));

      var treeDiv = el('div', {className: 'tree'});
      treeDiv.appendChild(buildFileTree(tree, '', right, nodeRegistry));
      left.appendChild(treeDiv);

      split.appendChild(left);
      split.appendChild(right);
      wrapper.appendChild(split);
      app.appendChild(wrapper);
      viewDom['files'] = wrapper;
    });
  }

  function buildFileTree(tree, prefix, rightPanel, registry) {
    var ul = el('ul');
    var keys = Object.keys(tree).sort();
    for (var k = 0; k < keys.length; k++) {
      var name = keys[k];
      var entry = tree[name];
      // Build fullPath to match DB paths exactly
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
            childUl = buildFileTree(entry._children, childPrefix, rightPanel, registry);
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
        // Try prefix search (for directories)
        var prefixUrl = '/api/files/by-prefix?prefix=' + encodeURIComponent(path);
        delete cache[prefixUrl];
        api(prefixUrl, function(d2) {
          container.innerHTML = '';
          if (!d2 || d2.length === 0) {
            container.appendChild(el('div', {style: 'color:var(--fg2)'}, 'No accesses found'));
            return;
          }
          var tbl = makeTable(['PID', 'Command', 'Mode', 'Duration', 'CPU', 'RSS', 'I/O', 'Filename'], d2.map(function(a) {
            var cpu = (a.user_time_us || 0) + (a.sys_time_us || 0);
            var io = (a.io_read_bytes || 0) + (a.io_write_bytes || 0);
            return [a.pid, shortenCmd(a.cmdline || '', 40), a.mode_str,
                    a.duration_us != null ? formatDuration(a.duration_us) : '-',
                    cpu > 0 ? formatDuration(cpu) : '-',
                    formatRss(a.peak_rss_kb || 0),
                    io > 0 ? formatBytes(io) : '-',
                    a.filename];
          }));
          container.appendChild(tbl);
        });
        return;
      }
      var tbl = makeTable(['PID', 'Command', 'Mode', 'Duration', 'CPU', 'RSS', 'I/O'], data.map(function(a) {
        var cpu = (a.user_time_us || 0) + (a.sys_time_us || 0);
        var io = (a.io_read_bytes || 0) + (a.io_write_bytes || 0);
        return [a.pid, shortenCmd(a.cmdline || '', 50), a.mode_str,
                a.duration_us != null ? formatDuration(a.duration_us) : '-',
                cpu > 0 ? formatDuration(cpu) : '-',
                formatRss(a.peak_rss_kb || 0),
                io > 0 ? formatBytes(io) : '-'];
      }));
      container.appendChild(tbl);
    });
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
      // Remove old
      var old = document.querySelector('.bottleneck-list');
      if (old) old.remove();

      data.sort(function(a, b) { return b[sortKey] - a[sortKey]; });

      var container = el('div', {className: 'bottleneck-list'});

      // Sort controls
      var sortRow = el('div', {style: 'margin-bottom:12px;color:var(--fg2)'}, 'Sort by: ');
      var sorts = [['total_us', 'Total Time'], ['count', 'Count'], ['max_us', 'Max Time'], ['avg_us', 'Avg Time']];
      for (var s = 0; s < sorts.length; s++) {
        (function(key, label) {
          var btn = el('span', {
            style: 'cursor:pointer;padding:4px 8px;margin-right:4px;border-radius:3px;' +
              (sortKey === key ? 'background:var(--border);color:var(--accent)' : ''),
            onclick: function() {
              sortKey = key;
              var app = document.getElementById('app');
              renderGroups(data, sortKey);
            }
          }, label);
          sortRow.appendChild(btn);
        })(sorts[s][0], sorts[s][1]);
      }
      container.appendChild(sortRow);

      // Headers
      var hdr = makeTable(['Command', 'Count', 'Total', 'Avg', 'Max'], data.map(function(g) {
        return [g.cmd_name, g.count, formatDuration(g.total_us), formatDuration(g.avg_us), formatDuration(g.max_us)];
      }));
      container.appendChild(hdr);

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
    app.innerHTML = '<div class="loading">Loading...</div>';

    var tabs = ['Critical Path', 'Hotspots', 'Failures', 'Diagnostics'];
    var activeTab = 0;

    app.innerHTML = '';
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
          var row = el('div', {style: 'padding:4px 8px;padding-left:' + (indent + 8) + 'px;border-left:2px solid var(--accent);margin-bottom:2px;background:var(--bg3);border-radius:0 4px 4px 0'}, [
            el('span', {style: 'color:var(--fg2)'}, '[' + p.pid + '] '),
            el('span', null, shortenCmd(p.cmdline, 60)),
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
          var tbl1 = makeTable(['Errno', 'Name', 'Count'], data.by_errno.map(function(e) {
            return [e.errno_val, e.name, e.count];
          }));
          content.appendChild(tbl1);
        }

        if (data.by_file && data.by_file.length > 0) {
          content.appendChild(el('div', {style: 'margin:12px 0 8px;font-weight:bold'}, 'Top Files'));
          var tbl2 = makeTable(['Count', 'File'], data.by_file.map(function(f) {
            return [f.count, f.filename];
          }));
          content.appendChild(tbl2);
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
    }
  }

  // ---- Navigation ----
  function switchView(view) {
    currentView = view;
    var tabs = document.querySelectorAll('#navbar .tab');
    for (var i = 0; i < tabs.length; i++) {
      tabs[i].className = 'tab' + (tabs[i].getAttribute('data-view') === view ? ' active' : '');
    }

    // Detach current cached view (don't destroy it)
    var app = document.getElementById('app');
    while (app.firstChild) app.removeChild(app.firstChild);

    // Cached views: processes and files preserve tree state
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

  // Init
  document.addEventListener('DOMContentLoaded', function() {
    var tabs = document.querySelectorAll('#navbar .tab');
    for (var i = 0; i < tabs.length; i++) {
      tabs[i].addEventListener('click', function(e) {
        e.preventDefault();
        switchView(this.getAttribute('data-view'));
      });
    }
    switchView('summary');
  });

  return { switchView: switchView };
})();
