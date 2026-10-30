# LYNX SHARE na Microsoft Store — passo a passo (pt-BR)

Tudo o que dá para preparar já está no repositório (pacote MSIX, textos, imagens). Falta o que só você pode fazer:
a conta e os cliques de envio. Detalhes técnicos: `docs/STORE.md`.

> **Feito (27/10):** conta criada, nome **LYNX SHARE** reservado e as variáveis do GitHub preenchidas (identidade
> `KA1WARE.LYNXSHARE`, Store ID `9NQRFBFDCX6N`). Siga a partir do passo 4. Os passos 1–3 ficam aqui para referência.

## 1. Criar a conta (grátis, uma vez)

1. Abra <https://storedeveloper.microsoft.com> → **Get started** / **Começar** → entre com sua conta Microsoft.
2. Escolha conta **Individual** (pessoa física). Confirme a identidade (documento + selfie). Sem taxa.
3. Espere o e-mail de verificação (pode levar alguns dias).

## 2. Reservar o nome

1. Partner Center → **Aplicativos e jogos** → **Novo produto** → **Aplicativo MSIX ou PWA**.
2. Nome: **LYNX SHARE** ("LYNX" já estava ocupado). O pacote já sai com esse nome na Store; desde a v0.1.20 o menu
   Iniciar e o próprio app também dizem "LYNX SHARE".

## 3. Copiar a identidade para o GitHub

1. No app → **Gerenciamento do produto** → **Identidade do produto**. Copie:
   `Package/Identity/Name`, `Package/Identity/Publisher`, `Package/Properties/PublisherDisplayName`.
2. GitHub → repositório LYNX SHARE → **Settings** → **Secrets and variables** → **Actions** → aba **Variables** →
   **New repository variable**, uma para cada:
   - `MSIX_IDENTITY_NAME` = Package/Identity/Name
   - `MSIX_PUBLISHER` = Package/Identity/Publisher (começa com `CN=`)
   - `MSIX_PUBLISHER_DISPLAY` = PublisherDisplayName
   - (`MSIX_DISPLAY_NAME` não precisa: o padrão já é `LYNX SHARE`; só crie se reservar outro nome)
3. Me avise: eu gero o pacote de novo (ou ele sai sozinho na próxima versão) — arquivo `LYNX-<versão>-store.msix`
   nos assets do release no GitHub.

## 4. Preencher o envio

No app → **Iniciar envio**:

| Seção | O que pôr |
|---|---|
| Preços e disponibilidade | **Gratuito**; mercados: todos; visibilidade: pública |
| Propriedades | Categoria **Música**; política de privacidade: `https://github.com/Ka1-S/LYNX-SHARE/blob/main/PRIVACY.md`; site `https://lynx-share.com`; suporte `https://github.com/Ka1-S/LYNX-SHARE/issues`. "Acessa, coleta ou transmite informações pessoais?" **Sim** — por segurança: o relatório de falha (só com consentimento) pode levar um e-mail se o usuário digitar; a política de privacidade cobre isso |
| Classificação etária | Questionário IARC: categoria **App (não jogo)**; violência, sexo, linguagem, drogas, apostas: **não**; os usuários interagem/compartilham conteúdo entre si: **não**; compartilha localização: **não**; compras digitais: **não**; navegador sem restrição: **não**. Resultado esperado: Livre / 3+ |
| Pacotes | Envie `LYNX-<versão>-store.msix` (baixado do release). Família: Windows 10/11 Desktop |
| Recursos restritos (runFullTrust) | Cole: *"LYNX SHARE is a desktop digital audio workstation (Win32/JUCE). It needs full trust to host the user's VST3/CLAP audio plugins, open ASIO/WASAPI audio drivers with low latency and read and write the user's project and sample files anywhere on disk."* |
| Listagens da Store | 16 idiomas prontos em `store/listings/` — o jeito rápido é importar tudo de uma vez (abaixo) |
| Opções de envio | Notas para a certificação: *"Free open-source DAW. No account or sign-in needed. Open Demo.lynx from the app folder or File › New to test."* |

### Listagens em 16 idiomas (importar de uma vez)

Textos prontos: inglês, português (Brasil), espanhol (Espanha, e o mesmo texto para o México), francês, alemão,
italiano, holandês, polonês, russo, turco, indonésio, japonês, coreano, chinês simplificado e tradicional. A interface
do LYNX SHARE tem esses 15 idiomas (desde a 0.1.20), e o pacote MSIX declara todos eles; o espanhol do México usa o texto da
Espanha.

1. No app → **Listagens da Store** → **Exportar listagem** → salve o `.csv` (ex.: em Downloads).
2. Me mande o caminho do arquivo — ou rode você mesmo, na pasta do repositório:
   `python scripts/store-listing-csv.py "<arquivo exportado>.csv" lynx-listings`
   (cria a pasta `lynx-listings` com o CSV preenchido + capturas + logos; avisa se algum campo não coube).
3. No app → **Importar listagens** → **Importar pasta** → escolha `lynx-listings`. Se aparecer erro, clique em
   **Exibir erros** e me mande o texto.

Sem importar: em **Gerenciar idiomas adicionais**, marque cada idioma e copie as seções do arquivo
`store/listings/<idioma>.md` (Título = **LYNX SHARE**, Descrição, Novidades, Recursos, Termos de pesquisa, legendas)
e envie as 5 capturas de `store/screenshots/`.

Clique **Enviar para a Store**. A certificação costuma levar de 1 a 3 dias úteis; se reprovarem algo, o e-mail diz o
quê — me mande o texto.

## 5. Depois que estiver no ar

- Me avise: eu ligo o botão **"Microsoft Store (recomendado)"** no site (já pronto no branch `wip/store165-site`,
  apontando para `https://apps.microsoft.com/detail/9NQRFBFDCX6N`) e publico.
- Novas versões: Partner Center → LYNX SHARE → **Atualizar** → Pacotes → envie o novo `.msix` → Novidades → Enviar.
- Opcional: Windows → Configurações → Sistema → Para desenvolvedores → **Modo de desenvolvedor** permite eu testar
  o pacote instalado neste PC sem pedir administrador (hoje está desligado; o teste foi feito no GitHub Actions).
