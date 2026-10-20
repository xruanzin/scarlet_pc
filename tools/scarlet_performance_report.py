#!/usr/bin/env python3
"""Summarize one Scarlet diagnostic session using only Python's standard library."""
import csv
import json
import math
from collections import defaultdict
from pathlib import Path
import re
import sys


def rows(path):
    if not path.exists():
        return []
    with path.open(encoding='utf-8-sig', errors='replace', newline='') as f:
        return list(csv.DictReader(f))


def number(value):
    try:
        n = float(value.replace(",", ".") if isinstance(value, str) else value)
        return n if math.isfinite(n) else None
    except (TypeError, ValueError):
        return None


def percentile(values, p):
    values = sorted(values)
    i = (len(values) - 1) * p
    lo = int(i)
    return values[lo] + (values[min(lo+1, len(values)-1)]-values[lo])*(i-lo)


def summarize(directory):
    d = Path(directory)
    lines = ['# Diagnostico de desempenho do Scarlet', '',
        'Rodada instrumentada: o profiler altera o custo de execucao. Compare FPS com outra rodada sem instrumentacao.', '',
        '## Como interpretar', '',
        '- Backend e HLE medem tempo de parede; incluem esperas e callbacks. Nao some categorias sobrepostas.',
        '- other_ms e o restante do intervalo entre presents, nao CPU ativa comprovada. Pode incluir pacing, escalonamento e espera.',
        '- cpu_cores=1 significa um processador logico ocupado; a porcentagem da maquina divide pelo numero de processadores logicos.',
        '- GPU por motor vem do Windows. Nao some percentuais de motores/instancias; Vulkan mede trabalho com timestamps em outro escopo.',
        '- Contagens de funcoes nao sao tempos. Threads sao identificadas por TID do Windows; nao ha captura de pilhas nem atribuicao de CPU por funcao.',
        '- Periodos sem presents nao aparecem como frames ate o proximo present; consulte processo, threads e alertas do log.',
        '- Arquivos ausentes significam coleta indisponivel/incompleta, nunca custo zero.', '']
    summary = {}
    for name in ['run.json', 'result.json']:
        if (d/name).exists():
            try:
                data = json.loads((d/name).read_text(encoding='utf-8-sig'))
                summary[name] = data
                lines += [f'## {name}', '', '```json', json.dumps(data, ensure_ascii=False, indent=2), '```', '']
            except (ValueError, OSError) as e:
                lines += [f'{name}: erro de leitura: {e}', '']
    frames = []
    for row in rows(d/'frames.csv'):
        nums = {k: number(v) for k,v in row.items() if k is not None}
        if all(nums.get(k) is not None for k in ('frame','frame_ms','backend_ms','gpu_wait_ms','present_ms','other_ms','draws','submits','shader_jobs')) and nums['frame_ms'] > 0:
            frames.append(nums)
    lines += ['## Linha do tempo de frames', '']
    if frames:
        ms = [r['frame_ms'] for r in frames]
        summary['frames'] = {'count':len(ms),'fps_over_intervals':1000*len(ms)/sum(ms),
                             'median_ms':percentile(ms,.5),'p95_ms':percentile(ms,.95),
                             'p99_ms':percentile(ms,.99),'max_ms':max(ms),
                             'above_33_34_ms':sum(x>33.34 for x in ms), 'above_50_ms':sum(x>50 for x in ms)}
        lines += [f'- Frames validos: {len(ms)}; FPS calculado nos intervalos: {1000*len(ms)/sum(ms):.2f}.',
                  f'- Mediana: {percentile(ms,.5):.2f} ms; p95: {percentile(ms,.95):.2f} ms; p99: {percentile(ms,.99):.2f} ms; maximo: {max(ms):.2f} ms.',
                  f'- Acima de 33,34 ms: {sum(x>33.34 for x in ms)}; acima de 50 ms: {sum(x>50 for x in ms)}.', '',
                  'Frames mais lentos (incluem loading e a janela de contagem de funcoes):', '',
                  '|Frame|Total ms|Backend ms|Espera GPU ms*|Present ms|Restante ms|Draws|Submits|Shaders pendentes|',
                  '|---|---|---|---|---|---|---|---|---|']
        for r in sorted(frames,key=lambda r:r['frame_ms'],reverse=True)[:30]:
            lines.append('|'+ '|'.join(f'{r[k]:.2f}' for k in ('frame','frame_ms','backend_ms','gpu_wait_ms','present_ms','other_ms','draws','submits','shader_jobs'))+'|')
        lines += ['', '*Espera GPU ja esta contida no backend. Veja frames.csv para a serie completa.', '']
    else:
        lines += ['Sem frames validos: jogo nao apresentou, runtime antigo, profiler desativado ou coleta interrompida.', '']
    lines += ['## Processo e threads (amostras do Windows)', '']
    for field in ('cpu_cores','cpu_machine_pct','working_set_mb','private_mb','threads'):
        vals = [n for r in rows(d/'process.csv') if (n:=number(r.get(field))) is not None]
        if vals:
            lines += [f'- {field}: media das amostras {sum(vals)/len(vals):.2f}, maximo {max(vals):.2f}.']
    thread_ms = defaultdict(float)
    for r in rows(d/'threads.csv'):
        n = number(r.get('cpu_ms'))
        if n is not None:
            thread_ms[(r.get('pid'),r.get('tid'))] += n
    lines += ['', '|PID/TID|CPU acumulada nos intervalos amostrados (ms)|','|---|---|']
    for (pid,tid), total in sorted(thread_ms.items(),key=lambda x:x[1],reverse=True)[:30]:
        lines += [f'|{pid}/{tid}|{total:.2f}|']
    lines += ['', 'Threads curtas que nasceram e morreram entre amostras podem faltar. CPU do processo e a medida mais abrangente.', '', '## GPU por motor', '']
    engines = defaultdict(list)
    for r in rows(d/'gpu-engines.csv'):
        n = number(r.get('utilization_pct'))
        if n is not None: engines[r.get('engine','?')].append(n)
    if engines:
        lines += ['|Motor|Media das amostras %|Maximo %|','|---|---|---|']
        for key, vals in sorted(engines.items(),key=lambda x:max(x[1]),reverse=True)[:30]:
            lines += [f'|{key}|{sum(vals)/len(vals):.2f}|{max(vals):.2f}|']
    else:
        lines += ['Contadores GPU indisponiveis ou sem amostras. Consulte monitor-errors.txt e os timestamps Vulkan no log.']
    log = (d/'scarlet_boot.log').read_text(encoding='utf-8-sig',errors='replace') if (d/'scarlet_boot.log').exists() else ''
    sync = defaultdict(lambda: {'wall_ms': 0.0, 'calls': 0})
    vk_barriers = {'full': 0, 'scoped_calls': 0, 'image_dependencies': 0, 'windows': 0}
    for line in log.splitlines():
        if line.startswith('[sync-prof]'):
            for name, ms, count in re.findall(r'([a-z-]+)=([\d.]+)ms/(\d+)', line):
                value = number(ms)
                if value is not None:
                    sync[name]['wall_ms'] += value
                    sync[name]['calls'] += int(count)
        if line.startswith('[vk-sync]'):
            m = re.search(r'full=(\d+) scoped-calls=(\d+) image-dependencies=(\d+)', line)
            if m:
                for key, value in zip(('full', 'scoped_calls', 'image_dependencies'), m.groups()):
                    vk_barriers[key] += int(value)
                vk_barriers['windows'] += 1
    summary['synchronization'] = dict(sync)
    summary['vulkan_barriers'] = vk_barriers if vk_barriers['windows'] else None
    lines += ['', '## Sincronizacao e apresentacao', '',
              'Tempos inclusivos de parede em intervalos concluidos, nao CPU ativa. Escopos se sobrepoem; nao some as linhas. A ultima janela pode faltar ao encerrar.', '']
    descriptions = {
        'big-lock': 'Espera para adquirir a trava global de gravacao',
        'present-gate': 'Espera por outra apresentacao/reinicio da janela',
        'interp-sleep': 'Espera deliberada entre imagens interpoladas, com a trava global liberada',
        'frame-begin': 'Preparacao do frame: slot GPU, swapchain e acquire',
        'acquire': 'vkAcquireNextImageKHR e eventual recriacao da swapchain',
        'queue-present': 'Tempo dentro de vkQueuePresentKHR (driver/compositor)',
        'backend-present': 'Apresentacao completa, incluindo locks e esperas internas',
        'guest-pace': 'Controle de ritmo pedido pelo jogo antes da fila de renderizacao',
        'present-ticket': 'Fila e execucao do present; inclui trabalhos anteriores e o backend',
        'gpu-wait': 'Espera de CPU pelo semaforo timeline da GPU',
    }
    if sync:
        lines += ['|Escopo|Parede registrada ms|Chamadas|Significado|', '|---|---|---|---|']
        for name, value in sorted(sync.items(), key=lambda item: item[1]['wall_ms'], reverse=True):
            lines += [f"|{name}|{value['wall_ms']:.3f}|{value['calls']}|{descriptions.get(name, 'Escopo do runtime')}|"]
        lines += ['', 'Contadores concorrentes podem cair em janelas vizinhas. GPU ociosa e tempo alto de present, sozinhos, nao identificam a causa.', '']
    else:
        lines += ['Sem contadores de espera separados: runtime antigo, perfil desligado ou janela incompleta. Nao equivale a tempo zero.', '']
    if vk_barriers['windows']:
        lines += [f"Barreiras dos helpers instrumentados em {vk_barriers['windows']} janelas: "
                  f"{vk_barriers['full']} completas; {vk_barriers['scoped_calls']} chamadas com escopo de imagem "
                  f"({vk_barriers['image_dependencies']} dependencias de imagens).",
                  'Transicoes de layout e outras barreiras locais fora desses helpers nao entram nesse total.', '']
    else:
        lines += ['Sem contagem das barreiras Vulkan emitidas pelos helpers nesta versao.', '']
    lines += ['`nvn-barrier-flags` (antigo `barrier`) conta comandos NVN e suas flags; flags zero nao chamam `vk_full_barrier` neste executor.',
              '`rdiscard` conta o estado de rasterizer discard, usado por transform feedback; nao mede passes jogados fora.', '']
    hle = defaultdict(lambda:[0.,0])
    for ms, count, name in re.findall(r'\[hle-prof\] ([\d.]+) ms (\d+) calls (\S+)',log):
        hle[name][0] += float(ms); hle[name][1] += int(count)
    lines += ['', '## Chamadas HLE', '',
              'Totais parciais: o runtime registra apenas o top 10 de cada intervalo. Tempos inclusivos de parede; podem se sobrepor. Nao e uma classificacao de CPU ativa.', '',
              '|Funcao|Parede registrada ms|Chamadas registradas|', '|---|---|---|']
    for name,(ms,count) in sorted(hle.items(),key=lambda x:x[1][0],reverse=True)[:40]:
        lines += [f'|{name}|{ms:.3f}|{count}|']
    funcs = []
    if (d/'fnprof.txt').exists():
        for line in (d/'fnprof.txt').read_text(errors='replace').splitlines():
            parts=line.split()
            if len(parts)==4:
                try: funcs.append((parts[0],int(parts[1]),parts[3]))
                except ValueError: pass
    lines += ['', '## Funcoes do jogo', '', 'Contagens apenas na janela configurada em run.json; enderecos guest, nao nomes nem tempos. Trampolines e alvos podem contar a mesma operacao.', '']
    if funcs:
        lines += ['|Endereco|Entradas|Primeiro caller LR|','|---|---|---|']
        for addr,count,lr in sorted(funcs,key=lambda r:r[1],reverse=True)[:50]:
            lines += [f'|{addr}|{count}|{lr}|']
    else:
        lines += ['Sem contagem: janela nao terminou, coleta desativada ou processo interrompido.']

    # Group contiguous slow periods by severity so analysis follows scenes, not absolute frame ids.
    def severity(ms):
        if ms >= 140: return 'critical'
        if ms >= 105: return 'severe'
        if ms >= 70: return 'slow'
        return None

    episodes = []
    cur = None
    for row in frames:
        sev = severity(row['frame_ms'])
        if sev is None:
            if cur:
                episodes.append(cur)
                cur = None
            continue
        rank = {'slow':1,'severe':2,'critical':3}[sev]
        if cur is None:
            cur = {'start':row['frame'],'end':row['frame'],'rank':rank,'rows':[row]}
        else:
            cur['end'] = row['frame']
            cur['rank'] = max(cur['rank'], rank)
            cur['rows'].append(row)
    if cur: episodes.append(cur)
    episodes = [e for e in episodes if len(e['rows']) >= 3]
    if episodes:
        names = {1:'slow',2:'severe',3:'critical'}
        lines += ['', '## Episodios de lentidao', '',
                  'Agrupados por lentidao sustentada; o numero absoluto do frame e apenas referencia interna.', '',
                  '|Episodio|Classe|Frames|Mediana ms|p95 ms|Backend mediano|Restante mediano|Draws medianos|',
                  '|---|---|---|---|---|---|---|---|']
        for idx,e in enumerate(episodes,1):
            rs=e['rows']; vals=[x['frame_ms'] for x in rs]
            med=lambda key: percentile([x[key] for x in rs],.5)
            lines += [f"|{idx}|{names[e['rank']]}|{len(rs)}|{percentile(vals,.5):.2f}|{percentile(vals,.95):.2f}|{med('backend_ms'):.2f}|{med('other_ms'):.2f}|{med('draws'):.0f}|"]
        summary['slow_episodes'] = [{
            'severity': names[e['rank']], 'frames': len(e['rows']),
            'median_ms': percentile([x['frame_ms'] for x in e['rows']], .5),
            'p95_ms': percentile([x['frame_ms'] for x in e['rows']], .95),
            'median_backend_ms': percentile([x['backend_ms'] for x in e['rows']], .5),
            'median_other_ms': percentile([x['other_ms'] for x in e['rows']], .5),
        } for e in episodes]

    hot = []
    for m in re.finditer(r'\[fnprof-hot\]\s+(\d+)\s+([0-9a-fA-F]+)\s+(.+?)\s+<-\s+([0-9a-fA-F]+)\s+(.+)', log, re.M):
        hot.append((int(m.group(1)), m.group(2), m.group(3), m.group(4), m.group(5)))
    if hot:
        lines += ['', '### Hotspots guest resolvidos', '',
                  'Nomes vindos das tabelas de simbolos NSO quando disponiveis; main pode estar parcialmente stripped.', '',
                  '|Entradas|Endereco|Funcao/simbolo|Caller LR|Caller/simbolo|',
                  '|---|---|---|---|---|']
        for count,addr,name,lr,caller in hot[:80]:
            lines += [f'|{count}|{addr}|{name}|{lr}|{caller}|']

    profile_files = sorted(d.glob('fnprof-auto-*.txt'))
    if profile_files:
        lines += ['', '### Capturas automaticas por severidade', '']
        for pf in profile_files:
            top = []
            for line in pf.read_text(errors='replace').splitlines()[:20]:
                parts=line.split()
                if len(parts)==4:
                    try: top.append((parts[0], int(parts[1]), parts[3]))
                    except ValueError: pass
            lines += [f'- {pf.name}: {len(top)} entradas de topo preservadas; veja o arquivo completo e fnprof-inspect.txt.']

    selected = [line for line in log.splitlines() if line.startswith(('[vk] t=','[gpuprof]','[fnprof]','[fnprof-auto]','[sync-prof]','[vk-sync]','[vk-sync-mode]'))]
    (d/'renderer-timeline.txt').write_text('\n'.join(selected)+'\n',encoding='utf-8')
    alerts = [line for line in log.splitlines() if re.search(r'FATAL|\[hang\]|no frame|no present|\[shader-slow\]|\[build-timeout\]|UNIMPLEMENTED|\[nvn-unimplemented\]|underrun|\*\*\* exception',line,re.I)]
    (d/'alerts.txt').write_text('\n'.join(alerts)+'\n',encoding='utf-8')
    lines += ['', '## Renderizacao, esperas e alertas', '',
              '- renderer-timeline.txt: tempos do backend por categoria, esperas, barreiras instrumentadas, GPU por intervalo e capturas esparsas de passes.',
              '- alerts.txt: erros, excecoes, travamentos, shaders lentos e chamadas nao implementadas encontradas no log.',
              '- fnprof-inspect.txt: simbolos e disassembly dos hotspots mais frequentes quando a coleta terminou.',
              '- scarlet_boot.log: fonte completa.',
              f'- Alertas encontrados: {len(alerts)}.', '',
              'Limites: o profiler altera o custo da janela em que esta ativo. Use as capturas para localizar hotspots, nao para medir FPS final.', '']
    (d/'report.md').write_text('\n'.join(lines),encoding='utf-8')
    (d/'summary.json').write_text(json.dumps(summary,ensure_ascii=False,indent=2),encoding='utf-8')
    return summary

if __name__ == '__main__':
    summarize(sys.argv[1])
    print('Report:', Path(sys.argv[1])/'report.md')
