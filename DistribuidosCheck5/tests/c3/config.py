"""Leitor do inventário CSV de Super Peers usado nos cenários C3."""
import csv
import ipaddress
from pathlib import Path


def load_superpeer_config(path):
    entries = []
    names = set()
    ports = set()
    with Path(path).open(newline="", encoding="utf-8") as source:
        for line, row in enumerate(csv.reader(source), start=1):
            if not row or not row[0].strip() or row[0].lstrip().startswith("#"):
                continue
            if len(row) != 6:
                raise ValueError(f"{path}:{line}: esperadas 6 colunas CSV")
            name, role, host, raw_port, field_five, field_six = (value.strip() for value in row)
            if not name or role.lower() != "superpeer":
                raise ValueError(f"{path}:{line}: nome vazio ou papel diferente de superpeer")
            try:
                ipaddress.IPv4Address(host)
                port = int(raw_port)
            except ValueError as error:
                raise ValueError(f"{path}:{line}: IP ou porta inválidos") from error
            if not 1 <= port <= 65535 or name in names or port in ports:
                raise ValueError(f"{path}:{line}: porta fora do intervalo ou entrada duplicada")
            names.add(name)
            ports.add(port)
            entries.append({"name": name, "role": role.lower(), "host": host, "port": port, "metadata": (field_five, field_six)})
    return entries
