#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Seed the wave-1 catalogs with the vocabulary a wallet user actually reads.

Covers navigation, actions, field labels, balances, states and error messages.
Long prose is left untranslated on purpose: it falls back to English, so a
screen reads as translated where it matters and English elsewhere, rather than
as a scatter of half-finished sentences.

Rules applied here, from qt/MULTI-LANGUAGE-PLAN.md:
  * UTXO and Covenants are universal and are never translated.
  * Where a language's own speakers use the English word, it is kept
    (Pool and Peers in Portuguese, Escrow in Spanish, and so on). Chinese and
    Russian localize most of these because their speakers do.
  * Amounts are untouched: no catalog changes number formatting.

These are unreviewed. Each language still needs its named reviewer before it
ships, which is the gate the plan sets.
"""

import io
import os
import re
import sys

COMMON = {
    # source: {lang: translation}
    "Overview": {"zh_CN": u"概览", "ru": u"Обзор", "pt_BR": u"Visão geral"},
    "Wallet": {"zh_CN": u"钱包", "ru": u"Кошелёк", "pt_BR": u"Carteira"},
    "Send": {"zh_CN": u"发送", "ru": u"Отправить", "pt_BR": u"Enviar"},
    "Receive": {"zh_CN": u"接收", "ru": u"Получить", "pt_BR": u"Receber"},
    "Transactions": {"zh_CN": u"交易", "ru": u"Транзакции", "pt_BR": u"Transações"},
    "Pay/Collect": {"zh_CN": u"支付/收款", "ru": u"Оплатить/Получить", "pt_BR": u"Pagar/Receber"},
    "Pool": {"zh_CN": u"矿池", "ru": u"Пул", "pt_BR": u"Pool"},
    "Shielded": {"zh_CN": u"屏蔽", "ru": u"Защищённые", "pt_BR": u"Blindado"},
    "Mining": {"zh_CN": u"挖矿", "ru": u"Майнинг", "pt_BR": u"Mineração"},
    "Settings": {"zh_CN": u"设置", "ru": u"Настройки", "pt_BR": u"Configurações"},
    "Hardware Wallet": {"zh_CN": u"硬件钱包", "ru": u"Аппаратный кошелёк", "pt_BR": u"Carteira de hardware"},
    "Payments": {"zh_CN": u"支付", "ru": u"Платежи", "pt_BR": u"Pagamentos"},
    "Escrow": {"zh_CN": u"托管", "ru": u"Эскроу", "pt_BR": u"Custódia"},
    "Marketplace": {"zh_CN": u"市场", "ru": u"Маркетплейс", "pt_BR": u"Mercado"},
    "Peers": {"zh_CN": u"对等节点", "ru": u"Пиры", "pt_BR": u"Peers"},
    "Template": {"zh_CN": u"模板", "ru": u"Шаблон", "pt_BR": u"Modelo"},
    "Liquidity Vault": {"zh_CN": u"流动性金库", "ru": u"Хранилище ликвидности", "pt_BR": u"Cofre de liquidez"},
    "Utreexo Proofs": {"zh_CN": u"Utreexo 证明", "ru": u"Доказательства Utreexo", "pt_BR": u"Provas Utreexo"},
    # actions
    "Cancel": {"zh_CN": u"取消", "ru": u"Отмена", "pt_BR": u"Cancelar"},
    "Close": {"zh_CN": u"关闭", "ru": u"Закрыть", "pt_BR": u"Fechar"},
    "Copy": {"zh_CN": u"复制", "ru": u"Копировать", "pt_BR": u"Copiar"},
    "Refresh": {"zh_CN": u"刷新", "ru": u"Обновить", "pt_BR": u"Atualizar"},
    "Clear": {"zh_CN": u"清除", "ru": u"Очистить", "pt_BR": u"Limpar"},
    "Stop": {"zh_CN": u"停止", "ru": u"Остановить", "pt_BR": u"Parar"},
    "Confirm": {"zh_CN": u"确认", "ru": u"Подтвердить", "pt_BR": u"Confirmar"},
    "Continue": {"zh_CN": u"继续", "ru": u"Продолжить", "pt_BR": u"Continuar"},
    "Search": {"zh_CN": u"搜索", "ru": u"Поиск", "pt_BR": u"Buscar"},
    "Details": {"zh_CN": u"详情", "ru": u"Подробности", "pt_BR": u"Detalhes"},
    "Help": {"zh_CN": u"帮助", "ru": u"Помощь", "pt_BR": u"Ajuda"},
    # labels
    "Amount:": {"zh_CN": u"金额：", "ru": u"Сумма:", "pt_BR": u"Valor:"},
    "Amount": {"zh_CN": u"金额", "ru": u"Сумма", "pt_BR": u"Valor"},
    "Amount (DIN):": {"zh_CN": u"金额 (DIN)：", "ru": u"Сумма (DIN):", "pt_BR": u"Valor (DIN):"},
    "From:": {"zh_CN": u"来自：", "ru": u"От:", "pt_BR": u"De:"},
    "To:": {"zh_CN": u"发送至：", "ru": u"Кому:", "pt_BR": u"Para:"},
    "Address:": {"zh_CN": u"地址：", "ru": u"Адрес:", "pt_BR": u"Endereço:"},
    "Address": {"zh_CN": u"地址", "ru": u"Адрес", "pt_BR": u"Endereço"},
    "Balance:": {"zh_CN": u"余额：", "ru": u"Баланс:", "pt_BR": u"Saldo:"},
    "Balance": {"zh_CN": u"余额", "ru": u"Баланс", "pt_BR": u"Saldo"},
    "Fee:": {"zh_CN": u"手续费：", "ru": u"Комиссия:", "pt_BR": u"Taxa:"},
    "Fee": {"zh_CN": u"手续费", "ru": u"Комиссия", "pt_BR": u"Taxa"},
    "Status:": {"zh_CN": u"状态：", "ru": u"Статус:", "pt_BR": u"Status:"},
    "Status": {"zh_CN": u"状态", "ru": u"Статус", "pt_BR": u"Status"},
    "Destination:": {"zh_CN": u"目标地址：", "ru": u"Назначение:", "pt_BR": u"Destino:"},
    "Provider:": {"zh_CN": u"服务商：", "ru": u"Провайдер:", "pt_BR": u"Provedor:"},
    "Password:": {"zh_CN": u"密码：", "ru": u"Пароль:", "pt_BR": u"Senha:"},
    "Password": {"zh_CN": u"密码", "ru": u"Пароль", "pt_BR": u"Senha"},
    "Confirm Password:": {"zh_CN": u"确认密码：", "ru": u"Подтвердите пароль:", "pt_BR": u"Confirmar senha:"},
    "Height:": {"zh_CN": u"高度：", "ru": u"Высота:", "pt_BR": u"Altura:"},
    "Date:": {"zh_CN": u"日期：", "ru": u"Дата:", "pt_BR": u"Data:"},
    "Type:": {"zh_CN": u"类型：", "ru": u"Тип:", "pt_BR": u"Tipo:"},
    "Label:": {"zh_CN": u"标签：", "ru": u"Метка:", "pt_BR": u"Rótulo:"},
    "Confirmations:": {"zh_CN": u"确认数：", "ru": u"Подтверждения:", "pt_BR": u"Confirmações:"},
    "Confirmations": {"zh_CN": u"确认数", "ru": u"Подтверждения", "pt_BR": u"Confirmações"},
    "Total:": {"zh_CN": u"总计：", "ru": u"Итого:", "pt_BR": u"Total:"},
    "Total": {"zh_CN": u"总计", "ru": u"Итого", "pt_BR": u"Total"},
    "Available:": {"zh_CN": u"可用：", "ru": u"Доступно:", "pt_BR": u"Disponível:"},
    "Available": {"zh_CN": u"可用", "ru": u"Доступно", "pt_BR": u"Disponível"},
    "Pending:": {"zh_CN": u"待确认：", "ru": u"В ожидании:", "pt_BR": u"Pendente:"},
    "Pending": {"zh_CN": u"待确认", "ru": u"В ожидании", "pt_BR": u"Pendente"},
    "Spendable:": {"zh_CN": u"可花费：", "ru": u"Доступно к трате:", "pt_BR": u"Disponível:"},
    "Immature:": {"zh_CN": u"未成熟：", "ru": u"Незрелые:", "pt_BR": u"Imaturo:"},
    "Unconfirmed:": {"zh_CN": u"未确认：", "ru": u"Неподтверждённые:", "pt_BR": u"Não confirmado:"},
    "Transaction ID:": {"zh_CN": u"交易 ID：", "ru": u"ID транзакции:", "pt_BR": u"ID da transação:"},
    "Recipient:": {"zh_CN": u"收款人：", "ru": u"Получатель:", "pt_BR": u"Destinatário:"},
    "Sender:": {"zh_CN": u"发送人：", "ru": u"Отправитель:", "pt_BR": u"Remetente:"},
    "Invoice ID:": {"zh_CN": u"账单 ID：", "ru": u"ID счёта:", "pt_BR": u"ID da fatura:"},
    "Invoice Details": {"zh_CN": u"账单详情", "ru": u"Детали счёта", "pt_BR": u"Detalhes da fatura"},
    # states and messages
    "Connecting...": {"zh_CN": u"正在连接…", "ru": u"Подключение…", "pt_BR": u"Conectando…"},
    "Connected": {"zh_CN": u"已连接", "ru": u"Подключено", "pt_BR": u"Conectado"},
    "Error": {"zh_CN": u"错误", "ru": u"Ошибка", "pt_BR": u"Erro"},
    "Warning": {"zh_CN": u"警告", "ru": u"Предупреждение", "pt_BR": u"Aviso"},
    "Invalid Input": {"zh_CN": u"输入无效", "ru": u"Неверный ввод", "pt_BR": u"Entrada inválida"},
    "Invalid Amount": {"zh_CN": u"金额无效", "ru": u"Неверная сумма", "pt_BR": u"Valor inválido"},
    "Invalid Address": {"zh_CN": u"地址无效", "ru": u"Неверный адрес", "pt_BR": u"Endereço inválido"},
    "Input Required": {"zh_CN": u"需要输入", "ru": u"Требуется ввод", "pt_BR": u"Entrada obrigatória"},
    "Please enter an amount.": {"zh_CN": u"请输入金额。", "ru": u"Введите сумму.", "pt_BR": u"Informe um valor."},
    "Amount must be greater than 0.": {"zh_CN": u"金额必须大于 0。", "ru": u"Сумма должна быть больше 0.", "pt_BR": u"O valor deve ser maior que 0."},
    "Copied to clipboard": {"zh_CN": u"已复制到剪贴板", "ru": u"Скопировано в буфер обмена", "pt_BR": u"Copiado para a área de transferência"},
    "Copy Invoice": {"zh_CN": u"复制账单", "ru": u"Копировать счёт", "pt_BR": u"Copiar fatura"},
    "Copy Package": {"zh_CN": u"复制数据包", "ru": u"Копировать пакет", "pt_BR": u"Copiar pacote"},
    "Conversion Successful": {"zh_CN": u"转换成功", "ru": u"Конвертация выполнена", "pt_BR": u"Conversão bem-sucedida"},
    "Conversion Failed": {"zh_CN": u"转换失败", "ru": u"Конвертация не удалась", "pt_BR": u"A conversão falhou"},
    "Enter amount to convert": {"zh_CN": u"输入要转换的金额", "ru": u"Введите сумму для конвертации", "pt_BR": u"Informe o valor a converter"},
    "API Key": {"zh_CN": u"API 密钥", "ru": u"Ключ API", "pt_BR": u"Chave de API"},
    "AI Settings": {"zh_CN": u"AI 设置", "ru": u"Настройки ИИ", "pt_BR": u"Configurações de IA"},
    # wallet flow
    "Wallet Name:": {"zh_CN": u"钱包名称：", "ru": u"Имя кошелька:", "pt_BR": u"Nome da carteira:"},
    "Wallet Fingerprint:": {"zh_CN": u"钱包指纹：", "ru": u"Отпечаток кошелька:", "pt_BR": u"Impressão digital da carteira:"},
    "First Address:": {"zh_CN": u"首个地址：", "ru": u"Первый адрес:", "pt_BR": u"Primeiro endereço:"},
    "Creating wallet...": {"zh_CN": u"正在创建钱包…", "ru": u"Создание кошелька…", "pt_BR": u"Criando carteira…"},
    "Wallet Setup Complete": {"zh_CN": u"钱包设置完成", "ru": u"Настройка кошелька завершена", "pt_BR": u"Configuração concluída"},
    "Your Dinero wallet is ready to use": {"zh_CN": u"您的 Dinero 钱包已可使用", "ru": u"Ваш кошелёк Dinero готов к использованию", "pt_BR": u"Sua carteira Dinero está pronta para uso"},
    "Your Seed Phrase": {"zh_CN": u"您的助记词", "ru": u"Ваша seed-фраза", "pt_BR": u"Sua frase semente"},
    "Confirm Your Seed Phrase": {"zh_CN": u"确认您的助记词", "ru": u"Подтвердите seed-фразу", "pt_BR": u"Confirme sua frase semente"},
    "Enter word...": {"zh_CN": u"输入单词…", "ru": u"Введите слово…", "pt_BR": u"Digite a palavra…"},
    "Incorrect Words": {"zh_CN": u"单词不正确", "ru": u"Неверные слова", "pt_BR": u"Palavras incorretas"},
    "Backup Required": {"zh_CN": u"需要备份", "ru": u"Требуется резервная копия", "pt_BR": u"Backup obrigatório"},
    "Import Required": {"zh_CN": u"需要导入", "ru": u"Требуется импорт", "pt_BR": u"Importação obrigatória"},
    "Import Complete": {"zh_CN": u"导入完成", "ru": u"Импорт завершён", "pt_BR": u"Importação concluída"},
    # language picker
    "Language": {"zh_CN": u"语言", "ru": u"Язык", "pt_BR": u"Idioma"},
    "Interface language:": {"zh_CN": u"界面语言：", "ru": u"Язык интерфейса:", "pt_BR": u"Idioma da interface:"},
    "Restart Dinero Now": {"zh_CN": u"立即重启 Dinero", "ru": u"Перезапустить Dinero", "pt_BR": u"Reiniciar o Dinero agora"},
    "Restart Now": {"zh_CN": u"立即重启", "ru": u"Перезапустить", "pt_BR": u"Reiniciar agora"},
    "Later": {"zh_CN": u"稍后", "ru": u"Позже", "pt_BR": u"Mais tarde"},
    "Restart Dinero": {"zh_CN": u"重启 Dinero", "ru": u"Перезапуск Dinero", "pt_BR": u"Reiniciar o Dinero"},
    "Language saved. Restart Dinero to apply it.": {
        "zh_CN": u"语言已保存。重启 Dinero 后生效。",
        "ru": u"Язык сохранён. Перезапустите Dinero, чтобы применить.",
        "pt_BR": u"Idioma salvo. Reinicie o Dinero para aplicar."},
    "Show Developer Menu": {"zh_CN": u"显示开发者菜单", "ru": u"Показать меню разработчика", "pt_BR": u"Mostrar menu do desenvolvedor"},
    "Hide Developer Menu": {"zh_CN": u"隐藏开发者菜单", "ru": u"Скрыть меню разработчика", "pt_BR": u"Ocultar menu do desenvolvedor"},
}

# Enforced-universal: written as themselves so the guard sees them explicitly.
UNIVERSAL = {"Covenants": u"Covenants", "UTXOs": u"UTXO"}


def esc(t):
    return t.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def seed(path, lang):
    text = io.open(path, encoding="utf-8").read()
    applied = 0
    pairs = [(s, m[lang]) for s, m in COMMON.items() if lang in m]
    pairs += list(UNIVERSAL.items())
    for source, target in pairs:
        pattern = re.compile(
            r"(<source>" + re.escape(esc(source)) +
            r"</source>\s*)<translation type=\"unfinished\"></translation>")
        text, n = pattern.subn(
            lambda m: m.group(1) + "<translation>" + esc(target) + "</translation>",
            text, count=1)
        applied += n
    io.open(path, "w", encoding="utf-8").write(text)
    return applied


def main():
    base = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "translations")
    for lang in ("zh_CN", "ru", "pt_BR"):
        path = os.path.normpath(os.path.join(base, "dinero_%s.ts" % lang))
        if not os.path.exists(path):
            print("missing catalog:", path)
            return 1
        print("%-6s seeded %d entries" % (lang, seed(path, lang)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
