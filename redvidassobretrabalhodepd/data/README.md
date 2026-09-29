# Fixtures

`generate_fixtures.sh` cria:
- small.txt
- exact_4MiB.bin
- over_4MiB.bin
- random_10MiB.bin
- documento_a.pdf
- documento_b.pdf
- documento_c.pdf
- SHA256SUMS

Os arquivos `.pdf` são fixtures de conteúdo, não PDFs semanticamente válidos;
isso é intencional: os testes de C2 verificam transporte, fragmentação,
compressão, recomposição e integridade, não interpretação do formato PDF.
