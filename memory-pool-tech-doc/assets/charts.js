// 内存池技术文档 —— 图表与流程图初始化
// 说明：Mermaid 负责结构图，ECharts 负责数据图；颜色全部取自 CSS 变量，保持与主题一致。
(function () {
  'use strict';

  // ---------- Mermaid：结构图 ----------
  if (window.mermaid) {
    window.mermaid.initialize({
      startOnLoad: true,
      theme: 'neutral',
      securityLevel: 'loose'
    });
  }

  // ---------- ECharts：数据图 ----------
  if (!window.echarts) return;

  var css = getComputedStyle(document.documentElement);
  function v(name) { return css.getPropertyValue(name).trim(); }

  var accent  = v('--accent');
  var accent2 = v('--accent2');
  var ink     = v('--ink');
  var muted   = v('--muted');
  var rule    = v('--rule');
  var family  = v('--font')     || "'InstrumentSans', 'Noto Sans SC', sans-serif";
  var mono    = v('--font-mono') || "'JetBrainsMono', monospace";

  var axisBase = {
    axisLine: { lineStyle: { color: rule } },
    axisTick: { show: false },
    axisLabel: { color: muted, fontFamily: mono, fontSize: 11 }
  };

  // ---------- 图 2：各对齐分段的档位数 ----------
  var elSize = document.getElementById('chart-sizeclass');
  if (elSize) {
    var chartSize = echarts.init(elSize, null, { renderer: 'svg' });
    chartSize.setOption({
      animation: false,
      textStyle: { fontFamily: family, color: ink },
      grid: { left: 8, right: 24, top: 32, bottom: 8, containLabel: true },
      tooltip: {
        trigger: 'axis',
        appendToBody: true,
        axisPointer: { type: 'shadow' },
        formatter: function (p) {
          var meta = [
            '对齐 16 B · 小对象最密',
            '对齐 128 B',
            '对齐 1 KB',
            '对齐 8 KB · 最疏'
          ];
          return p[0].axisValue + '<br/>' + meta[p[0].dataIndex] + '<br/><b>' + p[0].data + '</b> 档';
        }
      },
      xAxis: Object.assign({}, axisBase, {
        type: 'category',
        data: ['(0, 1KB]', '(1KB, 8KB]', '(8KB, 64KB]', '(64KB, 256KB]'],
        axisLabel: { color: muted, fontFamily: mono, fontSize: 11 }
      }),
      yAxis: {
        type: 'value',
        axisLine: { show: false },
        axisLabel: { color: muted, fontFamily: mono, fontSize: 11 },
        splitLine: { lineStyle: { color: rule } }
      },
      series: [{
        type: 'bar',
        data: [64, 56, 56, 24],
        barWidth: '48%',
        itemStyle: { color: accent, borderRadius: [4, 4, 0, 0] },
        label: { show: true, position: 'top', color: ink, fontWeight: 700, fontFamily: mono, fontSize: 12 }
      }]
    });
    window.addEventListener('resize', function () { chartSize.resize(); });
  }

  // ---------- 图 3：NumMoveSize 随规格变化 ----------
  var elMove = document.getElementById('chart-nummove');
  if (elMove) {
    var sizes = [16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536, 131072, 262144];
    var vals = sizes.map(function (s) {
      var n = Math.floor(262144 / s);
      if (n < 2) n = 2;
      if (n > 512) n = 512;
      return n;
    });
    var labels = sizes.map(function (s) { return s >= 1024 ? (s / 1024) + 'K' : String(s); });

    var chartMove = echarts.init(elMove, null, { renderer: 'svg' });
    chartMove.setOption({
      animation: false,
      textStyle: { fontFamily: family, color: ink },
      grid: { left: 8, right: 24, top: 32, bottom: 8, containLabel: true },
      tooltip: {
        trigger: 'axis',
        appendToBody: true,
        formatter: function (p) {
          return '请求 ' + p[0].axisValue + ' B<br/>一次批量领取 <b>' + p[0].data + '</b> 个';
        }
      },
      xAxis: Object.assign({}, axisBase, {
        type: 'category',
        data: labels,
        name: '规格（字节）',
        nameLocation: 'middle',
        nameGap: 42,
        nameTextStyle: { color: muted, fontFamily: family },
        axisLabel: { color: muted, fontFamily: mono, fontSize: 11, interval: 0, rotate: 40 }
      }),
      yAxis: {
        type: 'value',
        name: '个',
        nameTextStyle: { color: muted, fontFamily: family },
        axisLine: { show: false },
        axisLabel: { color: muted, fontFamily: mono, fontSize: 11 },
        splitLine: { lineStyle: { color: rule } }
      },
      series: [{
        type: 'line',
        data: vals,
        smooth: false,
        symbol: 'circle',
        symbolSize: 6,
        lineStyle: { color: accent, width: 2.5 },
        itemStyle: { color: accent },
        areaStyle: { color: accent + '1c' }
      }]
    });
    window.addEventListener('resize', function () { chartMove.resize(); });
  }
})();
