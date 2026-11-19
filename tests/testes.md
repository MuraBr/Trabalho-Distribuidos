- ping: recebe PONG.
- topology: mostra sucessor e predecessor. Com um único Super Peer, o sucessor deve ser ele mesmo.
- upload: termina com rc=0 e mostra ObjectID, Chunks e Compression: LZ4.
- lookup documento_a.pdf: consulta os metadados pelo nome.
- lookup <ObjectID>: consulta a rota Chord e mostra Lookup path e Owner.
- download: deve indicar Download completed e SHA-256 verified.
- quit: encerra a sessão do cliente.


bash cluster.sh check
bash cluster.sh deploy
bash cluster.sh up --clients
bash cluster.sh status --clients

bash cluster.sh logs SP1

bash cluster.sh console

C1 help
C1 ping
C1 topology
C1 upload ../files/documento_a.pdf
C1 lookup documento_a.pdf
C1 lookup <ObjectID>
C1 download documento_a.pdf resultado.pdf
C1 sleep 2


C1 join
C1 leave

client stop C1

@SP2 kill
status
@SP2 start
status