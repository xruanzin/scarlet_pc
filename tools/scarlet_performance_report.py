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
    selected = [line for line in log.splitlines() if line.startswith(('[vk] t=','[gpuprof]','[fnprof]'))]
    (d/'renderer-timeline.txt').write_text('\n'.join(selected)+'\n',encoding='utf-8')
    alerts = [line for line in log.splitlines() if re.search(r'FATAL|\[hang\]|no frame|no present|\[shader-slow\]|\[build-timeout\]|UNIMPLEMENTED|\[nvn-unimplemented\]|underrun',line,re.I)]
    (d/'alerts.txt').write_text('\n'.join(alerts)+'\n',encoding='utf-8')
    lines += ['', '## Renderizacao, esperas e alertas', '',
              '- renderer-timeline.txt: CPU do backend por categoria (desenho, descritores, texturas, uploads, pipelines), GPU por intervalo e capturas esparsas de passes, conforme disponibilidade do runtime.',
              '- alerts.txt: erros, travamentos, shaders lentos e chamadas nao implementadas encontradas no log.',
              '- scarlet_boot.log: fonte completa, incluindo carregamento, servicos e mensagens do kernel.',
              f'- Alertas encontrados: {len(alerts)}.', '',
              'Limites: nao coleta stacks de CPU, uso de cada instrucao, VRAM por recurso ou todas as chamadas com argumentos. Para atribuicao exata de CPU por funcao sera necessario um perfil por amostragem no Windows. Nao conclua gargalo apenas pela maior contagem.', '']
    (d/'report.md').write_text('\n'.join(lines),encoding='utf-8')
    (d/'summary.json').write_text(json.dumps(summary,ensure_ascii=False,indent=2),encoding='utf-8')
    return summary

if __name__ == '__main__':
    summarize(sys.argv[1])
    print('Report:', Path(sys.argv[1])/'report.md')
