# Integração do Checkpoint 3 - Alunos 1 e 2

Revisão: 07/10/2026. Os módulos dos dois alunos compõem os mesmos executáveis, com main exclusiva em peer.c e superpeer.c. Não há processo separado de Gossip ou Chord.

## Responsabilidades e contrato

| Parte | Responsabilidade |
| --- | --- |
| Aluno 1 | Membership, heartbeat, confirmação de falha, estados, Gossip e integração com o Peer |
| Aluno 2 | Overlay Chord, NodeID, sucessor/predecessor, fingers, roteamento, stabilize/notify/fix_fingers |
| Super Peer integrado | Recebe ambos os protocolos pelo mesmo listener TCP e usa o detector para atualizar o overlay |

A comunicação preserva o header de 98 bytes, CRC32 e TransactionID. HEARTBEAT/GOSSIP são 13/14; as mensagens Chord são 20-24. JOIN de Peer e ingresso no overlay continuam separados. Os formatos detalhados estão em checkpoint_3_aluno_1.md e continuidade_checkpoint_3_aluno_1.md.

O cadastro é implementado em membership.c, com APIs públicas em superpeer.h. Não copie para este projeto uma segunda implementação das funções superpeer_* dentro de superpeer.c: isso criaria símbolos duplicados. A versão atual de superpeer.c já chama o serviço heartbeat.c e o overlay chord.c/chord_network.c.

## Falha, resposta atrasada e reentrada

1. Heartbeat/sondagens identificam SUSPECT e confirmam FAILED; Gossip dissemina os eventos.
2. O callback de falha chama chord_forget: limpa sucessor/predecessor/fingers e bloqueia o NodeID para novas aplicações de respostas antigas.
3. chord_set_successor, chord_set_finger, chord_notify e chord_consider_predecessor verificam o bloqueio sob o mutex do Chord. chord_repair também ignora candidatos bloqueados.
4. O integrador busca candidatos ALIVE com incarnação não zero e evidência direta da instância atual. Um ALIVE recebido por rumor não herda a confirmação TCP de uma instância/estado anterior.
5. Somente após confirmar a recuperação, chord_allow libera o NodeID. O reparo escolhe o sucessor circular e a manutenção normal corrige o restante das fingers.

Antes da primeira confirmação do bootstrap, não desfazer o sucessor aprendido por JOIN Chord só porque ainda não há candidatos confirmados no snapshot. Um cadastro de Peer local não deve ser confundido com vizinho do overlay.

Nenhuma dessas operações locais faz TCP sob mutex de membership/Chord. A invalidação protege contra uma RPC iniciada antes da falha e concluída depois dela. As listas de bloqueio são liberadas ao destruir o Chord.

## Como compilar e testar

Na raiz do projeto:

```bash
make
make test-c3
python3 tests/c2/integration.py --bin-dir bin
```

make test-c3 compila os dois executáveis/aliases, executa as APIs locais de Chord/membership/controle, o teste TCP de três Super Peers e a suíte integrada de falhas. É possível usar BIN_DIR para builds isolados:

```bash
make BIN_DIR=/tmp/pd-c3-build test-c3
```

Execução manual, em terminais separados:

```bash
./bin/superpeer --port 55101
./bin/superpeer --port 55111 --chord-host 127.0.0.1 --chord-port 55101
./bin/superpeer --port 55121 --chord-host 127.0.0.1 --chord-port 55101
./bin/peer serve 55102 127.0.0.1 55101
./bin/peer upload arquivo.pdf
./bin/peer download arquivo.pdf recebido.pdf
```

O destino não pode existir. Preserve os diretórios de dados para demonstrar reentrada com NodeID estável. Para simular queda, use kill -9 somente no PID do processo iniciado por você. Os prazos padrão são 5/15/20/30 s; a suíte automática usa parâmetros acelerados e diretórios temporários exclusivos.

## Evidências desta integração

- Build C2x com warnings estritos e -Werror.
- make test-c3: APIs locais aprovadas; TCP Chord com três processos aprovado; suíte integrada com 17 verificações aprovada em /tmp/pd-c3-failures-pav_0usf.
- Repetição TCP após a guarda contra rumor ALIVE: 17/17 em /tmp/pd-c3-failures-2mz7i39u. Análise estática -fanalyzer/-Werror dos módulos chord.c e membership.c também passou nesta revisão.
- Integração C2: 46 verificações aprovadas em /tmp/pd-c2-integration-klmi2vws.
- A suíte integrada faz upload real e usa seu ObjectID para verificar o responsável Chord em todos os cinco Super Peers. Derruba o sucessor de um deles, confere 256 fingers sem o nó morto e verifica download íntegro durante o reparo.
- Reinicia o Super Peer morto preservando seu diretório, comprova NodeID estável/incarnação maior, verifica recuperação de membership e recompõe o anel de cinco nós.
- Builds isolados ASan/UBSan e ThreadSanitizer executaram testes locais e as 17 verificações TCP sem diagnósticos: evidências /tmp/pd-c3-failures-9afcpwjg e /tmp/pd-c3-failures-4x3oe13s. A guarda adicional contra rumor ALIVE após falha também foi verificada pelo teste local test_control na revisão final.

As execuções dos sanitizadores e seus diretórios correspondem aos comandos realmente executados; ausência de diagnósticos não prova todas as interleavings possíveis. As execuções de C1 10/10 e C2 próprio 20/20 documentadas no relatório anterior não foram repetidas neste complemento.

## Limites que permanecem

Chord encontra o Super Peer responsável por uma chave. Isso não transforma directory_lookup em consulta distribuída: os índices de documentos ainda são locais, sem migração/replicação. O download acima consulta o domínio no qual o documento foi anunciado; não promete consulta de metadados em qualquer Super Peer.

Não foram acrescentados eleição, consenso, SMR, 2PC, IST ou cache. O resultado negativo do roteiro disponível do professor para aliases de nomes com o mesmo ObjectID continua registrado no relatório anterior, assim como o diagnóstico da análise estática completa no metadata.c legado. Nenhum deles foi mascarado ou declarado resolvido por esta integração.
