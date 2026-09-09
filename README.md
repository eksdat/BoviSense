# BoviSense Environmental Control

Sistema embarcado de monitoramento ambiental e controle adaptativo de ventilação para instalações de bovinos leiteiros.

> Projeto acadêmico e experimental desenvolvido com ESP32, sensores ambientais e controle automático de ventilação, com foco em ambiência animal, eficiência energética e possibilidade de futura aplicação em instalações pecuárias reais.

---

## Sobre o projeto

A qualidade ambiental em instalações destinadas a bovinos leiteiros depende de diferentes fatores, incluindo temperatura, umidade relativa, ventilação, concentração de gases e características construtivas do galpão.

Sistemas de ventilação mecânica são utilizados para melhorar as condições ambientais, porém o funcionamento contínuo dos ventiladores pode resultar em consumo energético desnecessário em períodos nos quais a capacidade máxima de ventilação não é necessária.

Este projeto propõe o desenvolvimento de um sistema embarcado capaz de monitorar variáveis ambientais e controlar automaticamente a ventilação de acordo com as condições observadas.

A primeira versão será desenvolvida como uma prova de conceito em escala reduzida.

---

## Problema de pesquisa

A ventilação contínua pode proporcionar condições ambientais adequadas, porém pode consumir energia mesmo quando sua capacidade máxima não é necessária.

Por outro lado, reduzir a ventilação sem considerar as condições ambientais pode prejudicar o conforto térmico dos animais e a qualidade do ar.

A pergunta central do projeto é:

> Um sistema embarcado de controle adaptativo baseado em medições ambientais locais pode reduzir o tempo de operação e o consumo energético da ventilação sem aumentar significativamente o período em condições ambientais inadequadas?

---

## Objetivo geral

Desenvolver e avaliar um sistema embarcado de baixo custo para monitoramento ambiental e controle adaptativo da ventilação em instalações destinadas a bovinos leiteiros.

---

## Objetivos específicos

- monitorar temperatura do ambiente;
- monitorar a resposta de sensores relacionados à qualidade do ar;
- desenvolver um controlador baseado em ESP32;
- implementar diferentes níveis de ventilação;
- desenvolver sinalização local por display;
- registrar dados ambientais e operacionais;
- implementar estados de segurança em caso de falha;
- comparar diferentes estratégias de ventilação;
- avaliar consumo energético e tempo de operação;
- desenvolver uma arquitetura que possa futuramente ser escalada para instalações reais.
