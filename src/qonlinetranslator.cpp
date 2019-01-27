/*
 *  Copyright © 2018-2019 Hennadii Chernyshchyk <genaloner@gmail.com>
 *
 *  This file is part of QOnlineTranslator.
 *
 *  QOnlineTranslator is free library; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a get of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "qonlinetranslator.h"

#include <QEventLoop>
#include <QMediaPlayer>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>

// Engines have a limit of characters per translation request.
// If the query is larger, then it should be splited into several with getSplitIndex() helper function
constexpr int GOOGLE_TRANSLATE_LIMIT = 5000;
constexpr int GOOGLE_TTS_LIMIT = 200;

constexpr int YANDEX_TRANSLATE_LIMIT = 150;
constexpr int YANDEX_TRANSLIT_LIMIT = 180;
constexpr int YANDEX_TTS_LIMIT = 1400;

constexpr int BING_TRANSLATE_LIMIT = 5001;
constexpr int BING_TRANSLIT_LIMIT = 5000;
constexpr int BING_TTS_LIMIT = 2001;

QString QOnlineTranslator::m_yandexKey;
bool QOnlineTranslator::m_secondYandexKeyRequest = false;

const QStringList QOnlineTranslator::m_languageCodes = { "auto", "af", "sq", "am", "ar", "hy", "az", "eu", "ba", "be", "bn", "bs", "bg", "ca", "yue", "ceb", "zh-CN", "zh-TW", "co", "hr", "cs",
                                                         "da", "nl", "en", "eo", "et", "fj", "fil", "fi", "fr", "fy", "gl", "ka", "de", "el", "gu", "ht", "ha", "haw", "he", "mrj", "hi", "hmn", "hu",
                                                         "is", "ig", "id", "ga", "it", "ja", "jw", "kn", "kk", "km", "tlh", "tlh-Qaak", "ko", "ku", "ky", "lo", "la", "lv", "apc", "lt", "lb", "mk", "mg",
                                                         "ms", "ml", "mt", "mi", "mr", "mhr", "mn", "my", "ne", "no", "ny", "pap", "ps", "fa", "pl", "pt", "pa", "otq", "ro", "ru", "sm", "gd", "sr-Cyrl", "sr-Latin",
                                                         "st", "sn", "sd", "si", "sk", "sl", "so", "es", "su", "sw", "sv", "tl", "ty", "tg", "ta", "tt", "te", "th", "to", "tr", "udm", "uk", "ur", "uz",
                                                         "vi", "cy", "xh", "yi", "yo", "yua", "zu" };

QOnlineTranslator::QOnlineTranslator(QObject *parent) :
    QObject(parent)
{
}

void QOnlineTranslator::translate(const QString &text, Engine engine, Language translationLang, Language sourceLang, Language uiLang)
{
    // Set new data
    m_source = text;
    m_sourceLang = sourceLang;

    if (translationLang == Auto)
        m_translationLang = language(QLocale());
    else
        m_translationLang = translationLang;

    if (uiLang == Auto)
        m_uiLang = language(QLocale());
    else
        m_uiLang = uiLang;

    // Reset old data
    resetData();
    m_error = NoError;

    // Generate API codes
    QString sourceCode = translationLanguageCode(engine, m_sourceLang); // Not const, because it can be autodetected by engine
    if (sourceCode.isEmpty())
        return;

    const QString translationCode = translationLanguageCode(engine, m_translationLang);
    if (sourceCode.isEmpty())
        return;

    const QString uiCode = translationLanguageCode(engine, m_uiLang);
    if (uiCode.isEmpty())
        return;

    QString unsendedText;
    switch (engine) {
    case Google:
        unsendedText = m_source;
        while (!unsendedText.isEmpty()) {
            const int splitIndex = getSplitIndex(unsendedText, GOOGLE_TRANSLATE_LIMIT);

            // Do not translate the part if it looks like garbage
            if (splitIndex == -1) {
                m_translation.append(unsendedText.leftRef(GOOGLE_TRANSLATE_LIMIT));
                unsendedText = unsendedText.mid(GOOGLE_TRANSLATE_LIMIT);
                continue;
            }

            const QByteArray reply = getGoogleTranslation(unsendedText.left(splitIndex), translationCode, sourceCode, uiCode);
            if (reply.isEmpty())
                return;

            // Convert to JsonArray
            const QJsonDocument jsonResponse = QJsonDocument::fromJson(reply);
            const QJsonArray jsonData = jsonResponse.array();

            // Parse first sentense. If the answer contains more than one sentence, then at the end of the first one there will be a space
            m_translation.append(jsonData.at(0).toArray().at(0).toArray().at(0).toString());
            for (int i = 1; m_translation.endsWith(" ") || m_translation.endsWith("\n") || m_translation.endsWith(0x00a0); ++i)
                m_translation.append(jsonData.at(0).toArray().at(i).toArray().at(0).toString());

            // Parse transliterations and source language
            if (m_translationTranslitEnabled)
                m_translationTranslit.append(jsonData.at(0).toArray().last().toArray().at(2).toString());

            if (m_sourceTranslitEnabled)
                m_sourceTranslit.append(jsonData.at(0).toArray().last().toArray().at(3).toString());

            if (m_sourceLang == Auto) {
                // Parse language
                m_sourceLang = language(engine, jsonData.at(2).toString());
                if (m_sourceLang == NoLanguage)
                    return;
            }

            // Remove the parsed part from the next parsing
            unsendedText = unsendedText.mid(splitIndex);

            // Add a space between parts
            if (!unsendedText.isEmpty() && !m_translation.endsWith("\n")) {
                m_translation.append(" ");
                m_translationTranslit.append(" ");
                m_sourceTranslit.append(" ");
            }

            if (m_translationOptionsEnabled && text.size() < GOOGLE_TRANSLATE_LIMIT) {
                // Translation options
                foreach (const QJsonValue &typeOfSpeechData, jsonData.at(1).toArray()) {
                    m_translationOptions << QOption(typeOfSpeechData.toArray().at(0).toString());
                    foreach (const QJsonValue &wordData, typeOfSpeechData.toArray().at(2).toArray()) {
                        QString word = wordData.toArray().at(0).toString();
                        QString gender = wordData.toArray().at(4).toString();
                        QStringList translations;
                        foreach (const QJsonValue &wordTranslation, wordData.toArray().at(1).toArray()) {
                            translations.append(wordTranslation.toString());
                        }
                        m_translationOptions.last().addWord(word, gender, translations);
                    }
                }

                // Examples
                if (m_translationOptionsEnabled) {
                    foreach (const QJsonValue &exampleData, jsonData.at(12).toArray()) {
                        const QJsonArray example = exampleData.toArray().at(1).toArray().at(0).toArray();

                        m_examples << QExample(exampleData.toArray().at(0).toString());
                        m_examples.last().addExample(example.at(0).toString(), example.at(2).toString());
                    }
                }
            }
        }
        return;

    case Yandex:
        // Get translation
        unsendedText = m_source;
        while (!unsendedText.isEmpty()) {
            const int splitIndex = getSplitIndex(unsendedText, YANDEX_TRANSLATE_LIMIT);

            // Do not translate the part if it looks like garbage
            if (splitIndex == -1) {
                m_translation.append(unsendedText.leftRef(YANDEX_TRANSLATE_LIMIT));
                unsendedText = unsendedText.mid(YANDEX_TRANSLATE_LIMIT);
                continue;
            }

            // Get API reply
            const QByteArray reply = getYandexTranslation(unsendedText.left(splitIndex), translationCode, sourceCode);
            if (reply.isEmpty())
                return;

            // Parse translation data
            const QJsonDocument jsonResponse = QJsonDocument::fromJson(reply);
            const QJsonObject jsonData = jsonResponse.object();
            m_translation += jsonData.value("text").toArray().at(0).toString();
            if (m_sourceLang == Auto) {
                // Parse language
                sourceCode = jsonData.value("lang").toString();
                sourceCode = sourceCode.left(sourceCode.indexOf("-"));
                m_sourceLang = language(engine, sourceCode);
                if (m_sourceLang == NoLanguage)
                    return;
            }

            // Remove the parsed part from the next parsing
            unsendedText = unsendedText.mid(splitIndex);
        }

        // Get source transliteration
        if (m_sourceTranslitEnabled && isSupportYandexTranslit(m_sourceLang)) {
            unsendedText = m_source;
            while (!unsendedText.isEmpty()) {
                const int splitIndex = getSplitIndex(unsendedText, YANDEX_TRANSLIT_LIMIT);

                // Do not translate the part if it looks like garbage
                if (splitIndex == -1) {
                    m_sourceTranslit.append(unsendedText.leftRef(YANDEX_TRANSLIT_LIMIT));
                    unsendedText = unsendedText.mid(YANDEX_TRANSLIT_LIMIT);
                    continue;
                }

                // Get API reply
                const QByteArray reply = getYandexTranslit(unsendedText.left(splitIndex), sourceCode);
                if (reply.isEmpty())
                    return;

#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
                m_sourceTranslit += reply.mid(1).chopped(1);
#else
                m_sourceTranslit += reply.mid(1);
                m_sourceTranslit.chop(1);
#endif

                // Remove the parsed part from the next parsing
                unsendedText = unsendedText.mid(splitIndex);
            }
        }

        // Get translation transliteration
        if (m_sourceTranslitEnabled && isSupportYandexTranslit(m_translationLang)) {
            unsendedText = m_translation;
            while (!unsendedText.isEmpty()) {
                const int splitIndex = getSplitIndex(unsendedText, YANDEX_TRANSLIT_LIMIT);

                // Do not translate the part if it looks like garbage
                if (splitIndex == -1) {
                    m_translationTranslit.append(unsendedText.leftRef(YANDEX_TRANSLIT_LIMIT));
                    unsendedText = unsendedText.mid(YANDEX_TRANSLIT_LIMIT);
                    continue;
                }

                // Get API reply
                const QByteArray reply = getYandexTranslit(unsendedText.left(splitIndex), translationCode);
                if (reply.isEmpty())
                    return;

#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
                m_translationTranslit += reply.mid(1).chopped(1);
#else
                m_translationTranslit += reply.mid(1);
                m_translationTranslit.chop(1);
#endif

                // Remove the parsed part from the next parsing
                unsendedText = unsendedText.mid(splitIndex);
            }
        }

        // Request dictionary data if only one word is translated.
        if (m_translationOptionsEnabled && isSupportYandexDictionary(m_sourceLang, m_translationLang) && !m_source.contains(" ")) {
            const QByteArray reply = getYandexDictionary(m_source, translationCode, sourceCode, uiCode);
            if (reply.isEmpty())
                return;

            // Parse reply
            const QJsonDocument jsonResponse = QJsonDocument::fromJson(reply);
            const QJsonValue jsonData = jsonResponse.object().value(sourceCode + "-" + translationCode).toObject().value("regular");

            if (m_sourceTranscriptionEnabled)
                m_sourceTranscription = jsonData.toArray().at(0).toObject().value("ts").toString();

            foreach (const QJsonValue &typeOfSpeechData, jsonData.toArray()) {
                const QString typeOfSpeech = typeOfSpeechData.toObject().value("pos").toObject().value("text").toString();
                m_translationOptions << QOption(typeOfSpeech);

                foreach (const QJsonValue &wordData, typeOfSpeechData.toObject().value("tr").toArray()) {
                    // Parse translation options
                    const QString word = wordData.toObject().value("text").toString();
                    const QString gender = wordData.toObject().value("gen").toObject().value("text").toString();
                    QStringList translations;
                    foreach (const QJsonValue &wordTranslation, wordData.toObject().value("mean").toArray()) {
                        translations.append(wordTranslation.toObject().value("text").toString());
                    }

                    m_translationOptions.last().addWord(word, gender, translations);

                    // Parse examples
                    if (m_examplesEnabled && wordData.toObject().contains("ex")) {
                        // Check if no examples with this type of speech
                        if (m_examples.isEmpty() || m_examples.constLast().typeOfSpeech() != typeOfSpeech)
                            m_examples << QExample(typeOfSpeech);

                        foreach (const QJsonValue exampleData, wordData.toObject().value("ex").toArray()) {
                            const QString example = exampleData.toObject().value("text").toString();
                            const QString description = exampleData.toObject().value("tr").toArray().at(0).toObject().value("text").toString();

                            m_examples.last().addExample(description, example);
                        }
                    }
                }
            }
        }
        return;

    case Bing:
        // Get translation
        unsendedText = m_source;
        while (!unsendedText.isEmpty()) {
            const int splitIndex = getSplitIndex(unsendedText, BING_TRANSLATE_LIMIT);

            // Do not translate the part if it looks like garbage
            if (splitIndex == -1) {
                m_translation.append(unsendedText.leftRef(BING_TRANSLATE_LIMIT));
                unsendedText = unsendedText.mid(BING_TRANSLATE_LIMIT);
                continue;
            }

            // Detect language
            if (m_sourceLang == Auto) {
                sourceCode = getBingTextLanguage(unsendedText.left(splitIndex));
                if (sourceCode.isEmpty())
                    return;

                m_sourceLang = language(Bing, sourceCode);
            }

            // Get API reply
            const QByteArray reply = getBingTranslation(unsendedText.left(splitIndex), translationCode, sourceCode);
            if (reply.isEmpty())
                return;

            // Parse translation data
            m_translation += QJsonDocument::fromJson(reply).object().value("translationResponse").toString();

            // Remove the parsed part from the next parsing
            unsendedText = unsendedText.mid(splitIndex);
        }

        // Get source transliteration
        if (m_sourceTranslitEnabled && isSupportBingTranslit(m_sourceLang)) {
            unsendedText = m_source;
            while (!unsendedText.isEmpty()) {
                const int splitIndex = getSplitIndex(unsendedText, BING_TRANSLIT_LIMIT);

                // Do not translate the part if it looks like garbage
                if (splitIndex == -1) {
                    m_sourceTranslit.append(unsendedText.leftRef(BING_TRANSLIT_LIMIT));
                    unsendedText = unsendedText.mid(BING_TRANSLIT_LIMIT);
                    continue;
                }

                // Get API reply
                const QByteArray reply = getBingTranslit(unsendedText.left(splitIndex), sourceCode);
                if (reply.isEmpty())
                    return;

#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
                m_sourceTranslit += reply.mid(1).chopped(1);
#else
                m_sourceTranslit += reply.mid(1);
                m_sourceTranslit.chop(1);
#endif

                // Remove the parsed part from the next parsing
                unsendedText = unsendedText.mid(splitIndex);
            }
        }

        // Get translation transliteration
        if (m_translationTranslitEnabled && isSupportBingTranslit(m_translationLang)) {
            unsendedText = m_translation;
            while (!unsendedText.isEmpty()) {
                const int splitIndex = getSplitIndex(unsendedText, BING_TRANSLIT_LIMIT);

                // Do not translate the part if it looks like garbage
                if (splitIndex == -1) {
                    m_translationTranslit.append(unsendedText.leftRef(BING_TRANSLIT_LIMIT));
                    unsendedText = unsendedText.mid(BING_TRANSLIT_LIMIT);
                    continue;
                }

                // Get API reply
                const QByteArray reply = getBingTranslit(unsendedText.left(splitIndex), translationCode);
                if (reply.isEmpty())
                    return;

#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
                m_translationTranslit += reply.mid(1).chopped(1);
#else
                m_translationTranslit += reply.mid(1);
                m_translationTranslit.chop(1);
#endif

                // Remove the parsed part from the next parsing
                unsendedText = unsendedText.mid(splitIndex);
            }
        }

        // Request dictionary data if only one word is translated.
        if (m_translationOptionsEnabled && isSupportBingDictionary(m_sourceLang, m_translationLang) && !m_source.contains(" ")) {
            const QByteArray reply = getBingDictionary(m_source, translationCode, sourceCode);
            if (reply.isEmpty())
                return;

            // Parse reply
            const QJsonDocument jsonResponse = QJsonDocument::fromJson(reply);
            const QJsonValue jsonData = jsonResponse.object().value("translations");
            foreach (const QJsonValue &typeOfSpeechData, jsonData.toArray()) {
                const QString typeOfSpeech = typeOfSpeechData.toObject().value("posTag").toString().toLower();

                // Search for this type of speech
                int i;
                for (i = 0; i < m_translationOptions.size(); ++i) {
                    if (m_translationOptions.at(i).typeOfSpeech() == typeOfSpeech)
                        break;
                }

                // Add a new if current type of speech not found
                if (i == m_translationOptions.size())
                    m_translationOptions << QOption(typeOfSpeech);

                const QString word = typeOfSpeechData.toObject().value("displayTarget").toString().toLower();
                QStringList translations;
                foreach (const QJsonValue &wordTranslation, typeOfSpeechData.toObject().value("backTranslations").toArray())
                    translations.append(wordTranslation.toObject().value("displayText").toString());

                m_translationOptions[i].addWord(word, "", translations);
            }
        }

        return;
    }
}

QList<QMediaContent> QOnlineTranslator::media(const QString &text, Engine engine, Language lang, Voice voice, Emotion emotion)
{
    m_error = NoError;
    QList<QMediaContent> mediaList;
    QString langCode;

    // Detect language if required
    if (lang != Auto) {
        langCode = ttsLanguageCode(engine, lang);
        if (langCode.isEmpty())
            return mediaList;
    } else {
        switch (engine) {
        case Google: {
            // Get API reply
            const QByteArray reply = getGoogleTranslation(text.left(getSplitIndex(text, GOOGLE_TRANSLATE_LIMIT)), "en");
            if (reply.isEmpty())
                return mediaList;

            // Convert to JsonArray
            const QJsonDocument jsonResponse = QJsonDocument::fromJson(reply);
            const QJsonArray jsonData = jsonResponse.array();

            // Parse language
            langCode = jsonData.at(2).toString();
            break;
        }
        case Yandex: {
            // Get API reply
            const QByteArray reply = getYandexTranslation(text.left(getSplitIndex(text, YANDEX_TRANSLATE_LIMIT)), "en");
            if (reply.isEmpty())
                return mediaList;

            // Parse language
            const QJsonDocument jsonResponse = QJsonDocument::fromJson(reply);
            const QJsonObject jsonData = jsonResponse.object();
            langCode = jsonData.value("lang").toString();
            langCode = langCode.left(langCode.indexOf("-"));
            
            // Convert to tts code
            const Language detectedLang = language(Yandex, langCode);
            langCode = ttsLanguageCode(Yandex, detectedLang);
            if (langCode.isEmpty())
                return mediaList;
            break;
        }
        case Bing:
            // Get API reply
            langCode = getBingTextLanguage(text.left(getSplitIndex(text, BING_TRANSLATE_LIMIT)));
            if (langCode.isEmpty())
                return mediaList;

            // Convert to tts code
            const Language detectedLang = language(Bing, langCode);
            langCode = ttsLanguageCode(Bing, detectedLang);
            if (langCode.isEmpty())
                return mediaList;
        }
    }

    // Get speech
    QString unparsedText = text;
    switch (engine) {
    case Google:
    {
        const QString emotionCode = QOnlineTranslator::emotionCode(Google, emotion);
        if (emotionCode.isEmpty())
            return mediaList;

        const QString voiceCode = QOnlineTranslator::voiceCode(Google, voice);
        if (voiceCode.isEmpty())
            return mediaList;

        // Google has a limit of characters per tts request. If the query is larger, then it should be splited into several
        while (!unparsedText.isEmpty()) {
            const int splitIndex = getSplitIndex(unparsedText, GOOGLE_TTS_LIMIT); // Split the part by special symbol

            // Generate URL API for add it to the playlist
            QUrl apiUrl("http://translate.googleapis.com/translate_tts");
#if defined(Q_OS_LINUX)
            apiUrl.setQuery("ie=UTF-8&client=gtx&tl=" + langCode
                            + "&q=" + QUrl::toPercentEncoding(unparsedText.left(splitIndex)));
#elif defined(Q_OS_WIN)
            apiUrl.setQuery("ie=UTF-8&client=gtx&tl=" + langCode
                            + "&q=" + QUrl::toPercentEncoding(unparsedText.left(splitIndex)), QUrl::DecodedMode);
#endif
            mediaList.append(apiUrl);

            // Remove the said part from the next saying
            unparsedText = unparsedText.mid(splitIndex);
        }
        break;
    }
    case Yandex:
    {
        const QString emotionCode = QOnlineTranslator::emotionCode(Yandex, emotion);
        if (emotionCode.isEmpty())
            return mediaList;

        const QString voiceCode = QOnlineTranslator::voiceCode(Yandex, voice);
        if (voiceCode.isEmpty())
            return mediaList;

        // Yandex has a limit of characters per tts request. If the query is larger, then it should be splited into several
        while (!unparsedText.isEmpty()) {
            const int splitIndex = getSplitIndex(unparsedText, YANDEX_TTS_LIMIT); // Split the part by special symbol

            // Generate URL API for add it to the playlist
            QUrl apiUrl("https://tts.voicetech.yandex.net/tts");
#if defined(Q_OS_LINUX)
            apiUrl.setQuery("text=" + QUrl::toPercentEncoding(unparsedText.left(splitIndex))
                            + "&lang=" + langCode
                            + "&speaker=" + voiceCode
                            + "&emotion=" + emotionCode
                            + "&format=mp3");
#elif defined(Q_OS_WIN)
            apiUrl.setQuery("text=" + QUrl::toPercentEncoding(unparsedText.left(splitIndex))
                            + "&lang=" + langCode
                            + "&speaker=" + voiceCode
                            + "&emotion=" + emotionCode
                            + "&format=mp3", QUrl::DecodedMode);
#endif
            mediaList.append(apiUrl);

            // Remove the said part from the next saying
            unparsedText = unparsedText.mid(splitIndex);
        }
        break;
    }
    case Bing:
        const QString emotionCode = QOnlineTranslator::emotionCode(Bing, emotion);
        if (emotionCode.isEmpty())
            return mediaList;

        const QString voiceCode = QOnlineTranslator::voiceCode(Bing, voice);
        if (voiceCode.isEmpty())
            return mediaList;

        while (!unparsedText.isEmpty()) {
            const int splitIndex = getSplitIndex(unparsedText, BING_TTS_LIMIT); // Split the part by special symbol

            // Generate URL API for add it to the playlist
            QUrl apiUrl("https://www.bing.com/tspeak");
#if defined(Q_OS_LINUX)
            apiUrl.setQuery("&language=" + langCode
                            + "&text=" + QUrl::toPercentEncoding(unparsedText.left(splitIndex))
                            + "&options=" + voiceCode
                            + "&format=audio/mp3");
#elif defined(Q_OS_WIN)
            apiUrl.setQuery("&language=" + langCode
                            + "&text=" + QUrl::toPercentEncoding(unparsedText.left(splitIndex))
                            + "&options=" + voiceCode
                            + "&format=audio/mp3", QUrl::DecodedMode);
#endif
            mediaList.append(apiUrl);

            // Remove the said part from the next saying
            unparsedText = unparsedText.mid(splitIndex);
        }
    }

    return mediaList;
}

QList<QMediaContent> QOnlineTranslator::sourceMedia(Engine engine, Voice voice, Emotion emotion)
{
    return media(m_source, engine, m_sourceLang, voice, emotion);
}

QList<QMediaContent> QOnlineTranslator::translationMedia(Engine engine, Voice voice, Emotion emotion)
{
    return media(m_translation, engine, m_translationLang, voice, emotion);
}

QString QOnlineTranslator::source() const
{
    return m_source;
}

QString QOnlineTranslator::sourceTranslit() const
{
    return m_sourceTranslit;
}

QString QOnlineTranslator::sourceTranscription() const
{
    return m_sourceTranscription;
}

QString QOnlineTranslator::sourceLanguageString() const
{
    return languageString(m_sourceLang);
}

QOnlineTranslator::Language QOnlineTranslator::sourceLanguage() const
{
    return m_sourceLang;
}

QString QOnlineTranslator::translation() const
{
    return m_translation;
}

QString QOnlineTranslator::translationTranslit() const
{
    return m_translationTranslit;
}

QString QOnlineTranslator::translationLanguageString() const
{
    return languageString(m_translationLang);
}

QOnlineTranslator::Language QOnlineTranslator::translationLanguage() const
{
    return m_translationLang;
}

QList<QOption> QOnlineTranslator::translationOptions() const
{
    return m_translationOptions;
}

QList<QExample> QOnlineTranslator::examples() const
{
    return m_examples;
}

QString QOnlineTranslator::errorString() const
{
    return m_errorString;
}

QOnlineTranslator::TranslationError QOnlineTranslator::error() const
{
    return m_error;
}

QString QOnlineTranslator::languageString(QOnlineTranslator::Language lang)
{
    switch (lang) {
    case Auto:
        return tr("Automatically detect");
    case Afrikaans:
        return tr("Afrikaans");
    case Albanian:
        return tr("Albanian");
    case Amharic:
        return tr("Amharic");
    case Arabic:
        return tr("Arabic");
    case Armenian:
        return tr("Armenian");
    case Azerbaijani:
        return tr("Azeerbaijani");
    case Basque:
        return tr("Basque");
    case Bashkir:
        return tr("Bashkir");
    case Belarusian:
        return tr("Belarusian");
    case Bengali:
        return tr("Bengali");
    case Bosnian:
        return tr("Bosnian");
    case Bulgarian:
        return tr("Bulgarian");
    case Catalan:
        return tr("Catalan");
    case Cantonese:
        return tr("Cantonese");
    case Cebuano:
        return tr("Cebuano");
    case SimplifiedChinese:
        return tr("Chinese (Simplified)");
    case TraditionalChinese:
        return tr("Chinese (Traditional)");
    case Corsican:
        return tr("Corsican");
    case Croatian:
        return tr("Croatian");
    case Czech:
        return tr("Czech");
    case Danish:
        return tr("Danish");
    case Dutch:
        return tr("Dutch");
    case English:
        return tr("English");
    case Esperanto:
        return tr("Esperanto");
    case Estonian:
        return tr("Estonian");
    case Fijian:
        return tr("Fijian");
    case Filipino:
        return tr("Filipino");
    case Finnish:
        return tr("Finnish");
    case French:
        return tr("French");
    case Frisian:
        return tr("Frisian");
    case Galician:
        return tr("Galician");
    case Georgian:
        return tr("Georgian");
    case German:
        return tr("German");
    case Greek:
        return tr("Greek");
    case Gujarati:
        return tr("Gujarati");
    case HaitianCreole:
        return tr("Haitian Creole");
    case Hausa:
        return tr("Hausa");
    case Hawaiian:
        return tr("Hawaiian");
    case Hebrew:
        return tr("Hebrew");
    case HillMari:
        return tr("Hill Mari");
    case Hindi:
        return tr("Hindi");
    case Hmong:
        return tr("Hmong");
    case Hungarian:
        return tr("Hungarian");
    case Icelandic:
        return tr("Icelandic");
    case Igbo:
        return tr("Igbo");
    case Indonesian:
        return tr("Indonesian");
    case Irish:
        return tr("Irish");
    case Italian:
        return tr("Italian");
    case Japanese:
        return tr("Japanese");
    case Javanese:
        return tr("Javanese");
    case Kannada:
        return tr("Kannada");
    case Kazakh:
        return tr("Kazakh");
    case Khmer:
        return tr("Khmer");
    case Klingon:
        return tr("Klingon");
    case KlingonPlqaD:
        return tr("Klingon (PlqaD)");
    case Korean:
        return tr("Korean");
    case Kurdish:
        return tr("Kurdish");
    case Kyrgyz:
        return tr("Kyrgyz");
    case Lao:
        return tr("Lao");
    case Latin:
        return tr("Latin");
    case Latvian:
        return tr("Latvian");
    case LevantineArabic:
        return tr("Levantine Arabic");
    case Lithuanian:
        return tr("Lithuanian");
    case Luxembourgish:
        return tr("Luxembourgish");
    case Macedonian:
        return tr("Macedonian");
    case Malagasy:
        return tr("Malagasy");
    case Malay:
        return tr("Malay");
    case Malayalam:
        return tr("Malayalam");
    case Maltese:
        return tr("Maltese");
    case Maori:
        return tr("Maori");
    case Marathi:
        return tr("Marathi");
    case Mari:
        return tr("Mari");
    case Mongolian:
        return tr("Mongolian");
    case Myanmar:
        return tr("Myanmar");
    case Nepali:
        return tr("Nepali");
    case Norwegian:
        return tr("Norwegian");
    case Chichewa:
        return tr("Chichewa");
    case Papiamento:
        return tr("Papiamento");
    case Pashto:
        return tr("Pashto");
    case Persian:
        return tr("Persian");
    case Polish:
        return tr("Polish");
    case Portuguese:
        return tr("Portuguese");
    case Punjabi:
        return tr("Punjabi");
    case QueretaroOtomi:
        return tr("Queretaro Otomi");
    case Romanian:
        return tr("Romanian");
    case Russian:
        return tr("Russian");
    case Samoan:
        return tr("Samoan");
    case ScotsGaelic:
        return tr("Scots Gaelic");
    case SerbianCyrillic:
        return tr("Serbian (Cyrillic)");
    case SerbianLatin:
        return tr("Serbian (Latin)");
    case Sesotho:
        return tr("Sesotho");
    case Shona:
        return tr("Shona");
    case Sindhi:
        return tr("Sindhi");
    case Sinhala:
        return tr("Sinhala");
    case Slovak:
        return tr("Slovak");
    case Slovenian:
        return tr("Slovenian");
    case Somali:
        return tr("Somali");
    case Spanish:
        return tr("Spanish");
    case Sundanese:
        return tr("Sundanese");
    case Swahili:
        return tr("Swahili");
    case Swedish:
        return tr("Swedish");
    case Tagalog:
        return tr("Tagalog");
    case Tahitian:
        return tr("Tahitian");
    case Tajik:
        return tr("Tajik");
    case Tamil:
        return tr("Tamil");
    case Tatar:
        return tr("Tatar");
    case Telugu:
        return tr("Telugu");
    case Thai:
        return tr("Thai");
    case Tongan:
        return tr("Tongan");
    case Turkish:
        return tr("Turkish");
    case Udmurt:
        return tr("Udmurt");
    case Ukrainian:
        return tr("Ukrainian");
    case Urdu:
        return tr("Urdu");
    case Uzbek:
        return tr("Uzbek");
    case Vietnamese:
        return tr("Vietnamese");
    case Welsh:
        return tr("Welsh");
    case Xhosa:
        return tr("Xhosa");
    case Yiddish:
        return tr("Yiddish");
    case Yoruba:
        return tr("Yoruba");
    case YucatecMaya:
        return tr("Yucatec Maya");
    case Zulu:
        return tr("Zulu");
    default:
        return "";
    }
}

QString QOnlineTranslator::languageCode(QOnlineTranslator::Language lang)
{
    if (lang == NoLanguage)
        return "";

    return m_languageCodes.at(lang);
}

QOnlineTranslator::Language QOnlineTranslator::language(const QLocale &locale)
{
    switch (locale.language()) {
    case QLocale::Afrikaans:
        return Afrikaans;
    case QLocale::Albanian:
        return Albanian;
    case QLocale::Amharic:
        return Amharic;
    case QLocale::Arabic:
        return Arabic;
    case QLocale::Armenian:
        return Armenian;
    case QLocale::Azerbaijani:
        return Azerbaijani;
    case QLocale::Basque:
        return Basque;
    case QLocale::Belarusian:
        return Belarusian;
    case QLocale::Bengali:
        return Bengali;
    case QLocale::Bosnian:
        return Bosnian;
    case QLocale::Bulgarian:
        return Bulgarian;
    case QLocale::Catalan:
        return Catalan;
    case QLocale::Chinese:
        return SimplifiedChinese;
    case QLocale::LiteraryChinese:
        return TraditionalChinese;
    case QLocale::Corsican:
        return Corsican;
    case QLocale::Croatian:
        return Croatian;
    case QLocale::Czech:
        return Czech;
    case QLocale::Danish:
        return Danish;
    case QLocale::Dutch:
        return Dutch;
    case QLocale::Esperanto:
        return Esperanto;
    case QLocale::Estonian:
        return Estonian;
    case QLocale::Finnish:
        return Finnish;
    case QLocale::French:
        return French;
    case QLocale::Frisian:
        return Frisian;
    case QLocale::Galician:
        return Galician;
    case QLocale::Georgian:
        return Georgian;
    case QLocale::German:
        return German;
    case QLocale::Greek:
        return Greek;
    case QLocale::Gujarati:
        return Gujarati;
    case QLocale::Haitian:
        return HaitianCreole;
    case QLocale::Hausa:
        return Hausa;
    case QLocale::Hawaiian:
        return Hawaiian;
    case QLocale::Hebrew:
        return Hebrew;
    case QLocale::Hindi:
        return Hindi;
    case QLocale::HmongNjua:
        return Hmong;
    case QLocale::Hungarian:
        return Hungarian;
    case QLocale::Icelandic:
        return Icelandic;
    case QLocale::Igbo:
        return Igbo;
    case QLocale::Indonesian:
        return Indonesian;
    case QLocale::Irish:
        return Irish;
    case QLocale::Italian:
        return Italian;
    case QLocale::Japanese:
        return Japanese;
    case QLocale::Javanese:
        return Javanese;
    case QLocale::Kannada:
        return Kannada;
    case QLocale::Kazakh:
        return Kazakh;
    case QLocale::Khmer:
        return Khmer;
    case QLocale::Korean:
        return Korean;
    case QLocale::Kurdish:
        return Kurdish;
    case QLocale::Lao:
        return Lao;
    case QLocale::Latin:
        return Latin;
    case QLocale::Latvian:
        return Latvian;
    case QLocale::Lithuanian:
        return Lithuanian;
    case QLocale::Luxembourgish:
        return Luxembourgish;
    case QLocale::Macedonian:
        return Macedonian;
    case QLocale::Malagasy:
        return Malagasy;
    case QLocale::Malay:
        return Malay;
    case QLocale::Malayalam:
        return Malayalam;
    case QLocale::Maltese:
        return Maltese;
    case QLocale::Maori:
        return Maori;
    case QLocale::Marathi:
        return Marathi;
    case QLocale::Mongolian:
        return Mongolian;
    case QLocale::Nepali:
        return Nepali;
    case QLocale::Norwegian:
        return Norwegian;
    case QLocale::Pashto:
        return Pashto;
    case QLocale::Persian:
        return Persian;
    case QLocale::Polish:
        return Polish;
    case QLocale::Portuguese:
        return Portuguese;
    case QLocale::Punjabi:
        return Punjabi;
    case QLocale::Romanian:
        return Romanian;
    case QLocale::Russian:
        return Russian;
    case QLocale::Samoan:
        return Samoan;
    case QLocale::Gaelic:
        return ScotsGaelic;
    case QLocale::Serbian:
        return SerbianCyrillic;
    case QLocale::Shona:
        return Shona;
    case QLocale::Sindhi:
        return Sindhi;
    case QLocale::Sinhala:
        return Sinhala;
    case QLocale::Slovak:
        return Slovak;
    case QLocale::Slovenian:
        return Slovenian;
    case QLocale::Somali:
        return Somali;
    case QLocale::Spanish:
        return Spanish;
    case QLocale::Sundanese:
        return Sundanese;
    case QLocale::Swahili:
        return Swahili;
    case QLocale::Swedish:
        return Swedish;
    case QLocale::Filipino:
        return Filipino;
    case QLocale::Tajik:
        return Tajik;
    case QLocale::Tamil:
        return Tamil;
    case QLocale::Telugu:
        return Telugu;
    case QLocale::Thai:
        return Thai;
    case QLocale::Turkish:
        return Turkish;
    case QLocale::Ukrainian:
        return Ukrainian;
    case QLocale::Urdu:
        return Urdu;
    case QLocale::Uzbek:
        return Uzbek;
    case QLocale::Vietnamese:
        return Vietnamese;
    case QLocale::Welsh:
        return Welsh;
    case QLocale::Xhosa:
        return Xhosa;
    case QLocale::Yiddish:
        return Yiddish;
    case QLocale::Yoruba:
        return Yoruba;
    case QLocale::Zulu:
        return Zulu;
    default:
        return English;
    }
}

// Returns general language code
QOnlineTranslator::Language QOnlineTranslator::language(const QString &langCode)
{
    return static_cast<Language>(m_languageCodes.indexOf(langCode));
}

bool QOnlineTranslator::isSupportTranslation(QOnlineTranslator::Engine engine, QOnlineTranslator::Language lang)
{
    bool isSupported = false;

    switch (engine) {
    case Google:
        switch (lang) {
        case NoLanguage:
        case Bashkir:
        case HillMari:
        case Mari:
        case Papiamento:
        case Tatar:
        case Udmurt:
        case Cantonese:
        case Fijian:
        case Filipino:
        case Georgian:
        case Klingon:
        case KlingonPlqaD:
        case LevantineArabic:
        case QueretaroOtomi:
        case SerbianLatin:
        case Tahitian:
        case Tongan:
        case YucatecMaya:
            isSupported = false;
            break;
        default:
            isSupported = true;
            break;
        }
        break;
    case Yandex:
        switch (lang) {
        case NoLanguage:
        case LevantineArabic:
        case Cantonese:
        case Corsican:
        case Fijian:
        case Filipino:
        case Frisian:
        case Igbo:
        case Hausa:
        case Hawaiian:
        case Klingon:
        case KlingonPlqaD:
        case Kurdish:
        case Chichewa:
        case Pashto:
        case QueretaroOtomi:
        case Samoan:
        case SerbianLatin:
        case Sesotho:
        case Shona:
        case Sindhi:
        case Somali:
        case Tahitian:
        case Tongan:
        case Yoruba:
        case YucatecMaya:
        case Zulu:
            isSupported = false;
            break;
        default:
            isSupported = true;
            break;
        }
        break;
    case Bing:
        switch (lang) {
        case NoLanguage:
        case Albanian:
        case Amharic:
        case Armenian:
        case Azerbaijani:
        case Basque:
        case Bashkir:
        case Belarusian:
        case Cebuano:
        case Corsican:
        case Esperanto:
        case Frisian:
        case Galician:
        case Georgian:
        case Gujarati:
        case Hausa:
        case Hawaiian:
        case HillMari:
        case Igbo:
        case Irish:
        case Javanese:
        case Kannada:
        case Kazakh:
        case Khmer:
        case Kurdish:
        case Kyrgyz:
        case Lao:
        case Latin:
        case Luxembourgish:
        case Macedonian:
        case Malayalam:
        case Maori:
        case Marathi:
        case Mari:
        case Mongolian:
        case Myanmar:
        case Nepali:
        case Chichewa:
        case Papiamento:
        case Pashto:
        case Punjabi:
        case ScotsGaelic:
        case Sesotho:
        case Shona:
        case Sindhi:
        case Sinhala:
        case Somali:
        case Sundanese:
        case Tagalog:
        case Tajik:
        case Tatar:
        case Udmurt:
        case Uzbek:
        case Xhosa:
        case Yiddish:
        case Yoruba:
        case Zulu:
            isSupported = false;
            break;
        default:
            isSupported = true;
            break;
        }
    }

    return isSupported;
}

bool QOnlineTranslator::isSupportTts(QOnlineTranslator::Engine engine, QOnlineTranslator::Language lang)
{
    bool isSupported = false;

    switch (engine) {
    case Google:
        isSupported = isSupportTranslation(engine, lang); // Google use the same codes for tts
        break;
    case Yandex:
        switch (lang) {
        case Russian:
        case Tatar:
        case English:
            isSupported = true;
            break;
        default:
            isSupported = false;
            break;
        }
        break;
    case Bing:
        switch (lang) {
        case Arabic:
        case Catalan:
        case Danish:
        case German:
        case English:
        case Spanish:
        case Finnish:
        case French:
        case Hindi:
        case Italian:
        case Japanese:
        case Korean:
        case Norwegian:
        case Dutch:
        case Polish:
        case Portuguese:
        case Russian:
        case Swedish:
        case SimplifiedChinese:
        case TraditionalChinese:
            isSupported = true;
            break;
        default:
            isSupported = false;
            break;
        }
    }

    return isSupported;
}

QByteArray QOnlineTranslator::getGoogleTranslation(const QString &text, const QString &translationCode, const QString &sourceCode, const QString &uiCode)
{
    // Generate API url
    QUrl url("https://translate.googleapis.com/translate_a/single");
    url.setQuery("client=gtx&ie=UTF-8&oe=UTF-8&dt=bd&dt=ex&dt=ld&dt=md&dt=rw&dt=rm&dt=ss&dt=t&dt=at&dt=qc&sl=" + sourceCode
                 + "&tl=" + translationCode
                 + "&hl=" + uiCode
                 + "&q=" + QUrl::toPercentEncoding(text));

    // Send request and wait for the response
    QNetworkReply *reply = m_network.get(QNetworkRequest(url));
    QEventLoop waitForResponse;
    connect(reply, &QNetworkReply::finished, &waitForResponse, &QEventLoop::quit);
    waitForResponse.exec();

    if (reply->error() != QNetworkReply::NoError) {
        if (reply->error() == QNetworkReply::ServiceUnavailableError) {
            m_errorString = tr("Error: Engine systems have detected suspicious traffic from your computer network. Please try your request again later.");
            m_error = ServiceError;
        } else {
            m_errorString = reply->errorString();
            m_error = NetworkError;
        }
        delete reply;
        resetData();
        return "";
    }

    const QByteArray data = reply->readAll();
    delete reply;

    // Check availability of service
    if (data.startsWith("<")) {
        m_errorString = tr("Error: Engine systems have detected suspicious traffic from your computer network. Please try your request again later.");
        m_error = ServiceError;
        resetData();
        return "";
    }

    return data;
}

QByteArray QOnlineTranslator::getYandexTranslation(const QString &text, const QString &translationCode, const QString &sourceCode)
{
    // Generate session ID to access API (Required for Yandex)
    if (m_yandexKey.isEmpty()) {
        // Download web-version
        QNetworkReply *reply = m_network.get(QNetworkRequest(QUrl("https://translate.yandex.com/")));
        QEventLoop waitForResponse;
        connect(reply, &QNetworkReply::finished, &waitForResponse, &QEventLoop::quit);
        waitForResponse.exec();

        if (reply->error() != QNetworkReply::NoError) {
            m_errorString = reply->errorString();
            m_error = NetworkError;
            delete reply;
            resetData();
            return "";
        }

        const QByteArray webSiteData = reply->readAll();
        delete reply;

        // Check availability of service
        if (webSiteData.contains("<title>Oops!</title>") || webSiteData.contains("<title>302 Found</title>")) {
            m_errorString = tr("Error: Engine systems have detected suspicious traffic from your computer network. Please try your request again later.");
            m_error = ServiceError;
            resetData();
            return "";
        }

        // Parse SID
        const QString sid = webSiteData.mid(webSiteData.indexOf("SID: '") + 6, 26);

        // Yandex show reversed parts of session ID, need to decode
        QStringList sidParts = sid.split(".");
        for (short i = 0; i < sidParts.size(); ++i)
            std::reverse(sidParts[i].begin(), sidParts[i].end());

        m_yandexKey = sidParts.join(".");
        if (m_yandexKey.isEmpty()) {
            m_errorString = tr("Error: Unable to parse Yandex SID.");
            m_error = ParsingError;
            resetData();
            return "";
        }
    }

    // Generate API url
    QUrl url("https://translate.yandex.net/api/v1/tr.json/translate");
    url.setQuery("id="
                 + m_yandexKey
                 + "-0-0&srv=tr-text&text="
                 + QUrl::toPercentEncoding(text)
                 + "&lang="
                 + (sourceCode == "auto" ? translationCode : sourceCode + "-" + translationCode));

    // Send request and wait for the response
    QNetworkReply *reply = m_network.get(QNetworkRequest(url));
    QEventLoop waitForResponse;
    connect(reply, &QNetworkReply::finished, &waitForResponse, &QEventLoop::quit);
    waitForResponse.exec();

    // Check for errors
    if (reply->error() != QNetworkReply::NoError) {
        if (reply->error() < 201) {
            // Network errors
            m_errorString = reply->errorString();
            m_error = NetworkError;
            delete reply;
            resetData();
            return "";
        }

        // On error try to generate a new session ID second time, if the previous is invalid
        if (reply->error() == QNetworkReply::ContentAccessDenied && !m_secondYandexKeyRequest) {
            m_yandexKey.clear();
            m_secondYandexKeyRequest = true; // Do not generate the session ID third time if the second one was generated incorrectly
            delete reply;
            return getYandexTranslation(text, translationCode, sourceCode);
        }

        // Parse data to get request error type
        const QJsonDocument jsonResponse = QJsonDocument::fromJson(reply->readAll());
        m_errorString = jsonResponse.object().value("message").toString();
        m_error = ServiceError;
        delete reply;
        resetData();
        return "";
    }

    m_secondYandexKeyRequest = false;
    const QByteArray data = reply->readAll();
    delete reply;
    return data;
}

QByteArray QOnlineTranslator::getYandexTranslit(const QString &text, const QString &langCode)
{
    // Generate API url
    QUrl url("https://translate.yandex.net/translit/translit");
    url.setQuery("text=" + QUrl::toPercentEncoding(text)
                 + "&lang=" + langCode);

    // Send request and wait for the response
    QNetworkReply *reply = m_network.get(QNetworkRequest(url));
    QEventLoop waitForResponse;
    connect(reply, &QNetworkReply::finished, &waitForResponse, &QEventLoop::quit);
    waitForResponse.exec();

    if (reply->error() != QNetworkReply::NoError) {
        m_errorString = reply->errorString();
        m_error = NetworkError;
        delete reply;
        resetData();
        return "";
    }

    const QByteArray data = reply->readAll();
    delete reply;
    return data;
}

QByteArray QOnlineTranslator::getYandexDictionary(const QString &text, const QString &translationCode, const QString &sourceCode, const QString &uiCode)
{
    // Generate API url
    QUrl url("http://dictionary.yandex.net/dicservice.json/lookupMultiple");
    url.setQuery("text=" + QUrl::toPercentEncoding(text)
                 + "&ui=" + uiCode
                 + "&dict=" + sourceCode + "-" + translationCode);

    // Send request and wait for the response
    QNetworkReply *reply = m_network.get(QNetworkRequest(url));
    QEventLoop waitForResponse;
    connect(reply, &QNetworkReply::finished, &waitForResponse, &QEventLoop::quit);
    waitForResponse.exec();

    if (reply->error() != QNetworkReply::NoError) {
        m_errorString = reply->errorString();
        m_error = NetworkError;
        delete reply;
        resetData();
        return "";
    }

    const QByteArray data = reply->readAll();
    delete reply;
    return data;
}

QByteArray QOnlineTranslator::getBingTextLanguage(const QString &text)
{
    // Generate POST data
    QByteArray postData;
    postData.append("&text=" + text);

    // Generate POST request
    QNetworkRequest request;
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    request.setHeader(QNetworkRequest::ContentLengthHeader, postData.size());
    request.setUrl(QUrl("http://www.bing.com/tdetect"));

    // Send request and wait for the response
    QNetworkReply *reply = m_network.post(request, postData);
    QEventLoop waitForResponse;
    connect(reply, &QNetworkReply::finished, &waitForResponse, &QEventLoop::quit);
    waitForResponse.exec();

    // Check for errors
    if (reply->error() != QNetworkReply::NoError) {
        m_errorString = reply->errorString();
        m_error = NetworkError;
        delete reply;
        resetData();
        return "";
    }

    const QByteArray data = reply->readAll();
    delete reply;
    return data;
}

QByteArray QOnlineTranslator::getBingTranslation(const QString &text, const QString &translationCode, const QString &sourceCode)
{
    // Generate POST data
    QByteArray postData;
    postData.append("&text=" + text + "&from=" + sourceCode + "&to=" + translationCode);

    // Generate POST request
    QNetworkRequest request;
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    request.setHeader(QNetworkRequest::ContentLengthHeader, postData.size());
    request.setUrl(QUrl("http://www.bing.com/ttranslate"));

    // Send request and wait for the response
    QNetworkReply *reply = m_network.post(request, postData);
    QEventLoop waitForResponse;
    connect(reply, &QNetworkReply::finished, &waitForResponse, &QEventLoop::quit);
    waitForResponse.exec();

    // Check for errors
    if (reply->error() != QNetworkReply::NoError) {
        m_errorString = reply->errorString();
        m_error = NetworkError;
        delete reply;
        resetData();
        return "";
    }

    const QByteArray data = reply->readAll();
    delete reply;
    return data;
}

QByteArray QOnlineTranslator::getBingTranslit(const QString &text, const QString &langCode)
{
    // Generate POST data
    QByteArray postData;
    postData.append("&text=" + text + "&language=" + langCode + "&toScript=latn");

    // Generate POST request
    QNetworkRequest request;
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    request.setHeader(QNetworkRequest::ContentLengthHeader, postData.size());
    request.setUrl(QUrl("http://www.bing.com/ttransliterate"));

    // Send request and wait for the response
    QNetworkReply *reply = m_network.post(request, postData);
    QEventLoop waitForResponse;
    connect(reply, &QNetworkReply::finished, &waitForResponse, &QEventLoop::quit);
    waitForResponse.exec();

    // Check for errors
    if (reply->error() != QNetworkReply::NoError) {
        m_errorString = reply->errorString();
        m_error = NetworkError;
        delete reply;
        resetData();
        return "";
    }

    const QByteArray data = reply->readAll();
    delete reply;
    return data;
}

QByteArray QOnlineTranslator::getBingDictionary(const QString &text, const QString &translationCode, const QString &sourceCode)
{
    // Generate POST data
    QByteArray postData;
    postData.append("&text=" + text + "&from=" + sourceCode + "&to=" + translationCode);

    // Generate POST request
    QNetworkRequest request;
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    request.setHeader(QNetworkRequest::ContentLengthHeader, postData.size());
    request.setUrl(QUrl("http://www.bing.com/ttranslationlookup"));

    // Send request and wait for the response
    QNetworkReply *reply = m_network.post(request, postData);
    QEventLoop waitForResponse;
    connect(reply, &QNetworkReply::finished, &waitForResponse, &QEventLoop::quit);
    waitForResponse.exec();

    // Check for errors
    if (reply->error() != QNetworkReply::NoError) {
        m_errorString = reply->errorString();
        m_error = NetworkError;
        delete reply;
        resetData();
        return "";
    }

    const QByteArray data = reply->readAll();
    delete reply;
    return data;
}

// Returns engine-specific language code for translation
QString QOnlineTranslator::translationLanguageCode(Engine engine, QOnlineTranslator::Language lang)
{
    if (!isSupportTranslation(engine, lang)) {
        m_errorString = tr("Error: Selected language for translation is not supported by this engine.");
        m_error = ParametersError;
        resetData();
        return "";
    }

    // Engines have some language codes exceptions
    switch (engine) {
    case Google:
        if (lang == SerbianCyrillic)
            return "sr";
        break;
    case Yandex:
        switch (lang) {
        case SimplifiedChinese:
            return "zn";
        case Javanese:
            return "jv";
        case SerbianCyrillic:
            return "sr";
        default:
            break;
        }
        break;
    case Bing:
        switch (lang) {
        case SimplifiedChinese:
            return "zh-Hans";
        case TraditionalChinese:
            return "zh-Hant";
        case Hmong:
            return "mww";
        default:
            break;
        }
        break;
    }

    // General case
    return m_languageCodes.at(lang);
}

// Returns engine-specific language code for tts
QString QOnlineTranslator::ttsLanguageCode(QOnlineTranslator::Engine engine, QOnlineTranslator::Language lang)
{
    switch (engine) {
    case Google:
        return translationLanguageCode(engine, lang); // Google use the same codes for tts
    case Yandex:
        switch (lang) {
        case Russian:
            return "ru_RU";
        case Tatar:
            return "tr_TR";
        case English:
            return "en_GB";
        default:
            break;
        }
        break;
    case Bing:
        switch (lang) {
        case Arabic:
            return "ar-EG";
        case Catalan:
            return "ca-ES";
        case Danish:
            return "da-DK";
        case German:
            return "de-DE";
        case English:
            return "en-GB";
        case Spanish:
            return "es-ES";
        case Finnish:
            return "fi-FI";
        case French:
            return "fr-FR";
        case Hindi:
            return "hi-IN";
        case Italian:
            return "it-IT";
        case Japanese:
            return "ja-JP";
        case Korean:
            return "ko-KR";
        case Norwegian:
            return "nb-NO";
        case Dutch:
            return "nl-NL";
        case Polish:
            return "pl-PL";
        case Portuguese:
            return "pt-PT";
        case Russian:
            return "ru-RU";
        case Swedish:
            return "sv-SE";
        case SimplifiedChinese:
            return "zh-CN";
        case TraditionalChinese:
            return "zh-HK";
        default:
            break;
        }
    }

    m_errorString = tr("Error: Selected language for tts is not supported by this engine.");
    m_error = ParametersError;
    resetData();
    return "";
}

QString QOnlineTranslator::voiceCode(QOnlineTranslator::Engine engine, QOnlineTranslator::Voice voice)
{
    switch (engine) {
    case Google:
        if (voice == DefaultVoice)
            return "default";
        break;
    case Yandex:
        switch (voice) {
        case DefaultVoice:
        case Zahar:
            return "zahar";
        case Ermil:
            return "ermil";
        case Jane:
            return "jane";
        case Oksana:
            return "oksana";
        case Alyss:
            return "alyss";
        case Omazh:
            return "omazh";
        default:
            break;
        }
        break;
    case Bing:
        switch (voice) {
        case DefaultVoice:
        case Male:
            return "male";
        case Female:
            return "female";
        default:
            break;
        }
        break;
    }

    m_errorString = tr("Error: Selected voice is not supported by this engine.");
    m_error = ParametersError;
    resetData();
    return "";
}

QString QOnlineTranslator::emotionCode(Engine engine, QOnlineTranslator::Emotion emotion)
{
    switch (engine) {
    case Google:
    case Bing:
        if (emotion == DefaultEmotion)
            return "default";
        break;
    case Yandex:
        switch (emotion) {
        case DefaultEmotion:
        case Neutral:
            return "neutral";
        case Good:
            return "good";
        case Evil:
            return "evil";
        }
    }

    m_errorString = tr("Error: Selected emotion is not supported by this engine.");
    m_error = ParametersError;
    resetData();
    return "";
}

bool QOnlineTranslator::isSupportGoogle(QOnlineTranslator::Language lang)
{
    switch (lang) {
    case NoLanguage:
    case Bashkir:
    case HillMari:
    case Mari:
    case Papiamento:
    case Tatar:
    case Udmurt:
    case Cantonese:
    case Fijian:
    case Filipino:
    case Georgian:
    case Klingon:
    case KlingonPlqaD:
    case LevantineArabic:
    case QueretaroOtomi:
    case SerbianLatin:
    case Tahitian:
    case Tongan:
    case YucatecMaya:
        return false;
    default:
        return true;
    }
}

bool QOnlineTranslator::isSupportYandexTranslation(QOnlineTranslator::Language lang)
{
    switch (lang) {
    case NoLanguage:
    case LevantineArabic:
    case Cantonese:
    case Corsican:
    case Fijian:
    case Filipino:
    case Frisian:
    case Igbo:
    case Hausa:
    case Hawaiian:
    case Klingon:
    case KlingonPlqaD:
    case Kurdish:
    case Chichewa:
    case Pashto:
    case QueretaroOtomi:
    case Samoan:
    case SerbianLatin:
    case Sesotho:
    case Shona:
    case Sindhi:
    case Somali:
    case Tahitian:
    case Tongan:
    case Yoruba:
    case YucatecMaya:
    case Zulu:
        return false;
    default:
        return true;
    }
}

bool QOnlineTranslator::isSupportYandexTranslit(QOnlineTranslator::Language lang)
{
    switch (lang) {
    case Amharic:
    case Armenian:
    case Bengali:
    case SimplifiedChinese:
    case Georgian:
    case Greek:
    case Gujarati:
    case Hebrew:
    case Hindi:
    case Japanese:
    case Kannada:
    case Korean:
    case Malayalam:
    case Marathi:
    case Nepali:
    case Punjabi:
    case Russian:
    case Sinhala:
    case Tamil:
    case Telugu:
    case Thai:
    case Yiddish:
        return true;
    default:
        return false;
    }
}

bool QOnlineTranslator::isSupportYandexDictionary(QOnlineTranslator::Language sourceLang, Language translationLang)
{
    switch (sourceLang) {
    case Belarusian:
        switch (translationLang) {
        case Belarusian:
            return true;
        case Russian:
            return true;
        default:
            return false;
        }
    case Bulgarian:
        switch (translationLang) {
        case Russian:
            return true;
        default:
            return false;
        }
    case Czech:
        switch (translationLang) {
        case English:
            return true;
        case Russian:
            return true;
        default:
            return false;
        }
    case Danish:
        switch (translationLang) {
        case English:
            return true;
        case Russian:
            return true;
        default:
            return false;
        }
    case German:
        switch (translationLang) {
        case German:
            return true;
        case English:
            return true;
        case Russian:
            return true;
        case Turkish:
            return true;
        default:
            return false;
        }
    case Greek:
        switch (translationLang) {
        case English:
            return true;
        case Russian:
            return true;
        default:
            return false;
        }
    case English:
        switch (translationLang) {
        case Czech:
            return true;
        case Danish:
            return true;
        case German:
            return true;
        case Greek:
            return true;
        case English:
            return true;
        case Spanish:
            return true;
        case Estonian:
            return true;
        case Finnish:
            return true;
        case French:
            return true;
        case Italian:
            return true;
        case Lithuanian:
            return true;
        case Latvian:
            return true;
        case Dutch:
            return true;
        case Norwegian:
            return true;
        case Portuguese:
            return true;
        case Russian:
            return true;
        case Slovak:
            return true;
        case Swedish:
            return true;
        case Turkish:
            return true;
        case Ukrainian:
            return true;
        default:
            return false;
        }
    case Spanish:
        switch (translationLang) {
        case English:
            return true;
        case Spanish:
            return true;
        case Russian:
            return true;
        default:
            return false;
        }
    case Estonian:
        switch (translationLang) {
        case English:
            return true;
        case Russian:
            return true;
        default:
            return false;
        }
    case Finnish:
        switch (translationLang) {
        case English:
            return true;
        case Russian:
            return true;
        case Finnish:
            return true;
        default:
            return false;
        }
    case French:
        switch (translationLang) {
        case French:
            return true;
        case English:
            return true;
        case Russian:
            return true;
        default:
            return false;
        }
    case Hungarian:
        switch (translationLang) {
        case Hungarian:
            return true;
        case Russian:
            return true;
        default:
            return false;
        }
    case Italian:
        switch (translationLang) {
        case English:
            return true;
        case Italian:
            return true;
        case Russian:
            return true;
        default:
            return false;
        }
    case Lithuanian:
        switch (translationLang) {
        case English:
            return true;
        case Lithuanian:
            return true;
        case Russian:
            return true;
        default:
            return false;
        }
    case Latvian:
        switch (translationLang) {
        case English:
            return true;
        case Russian:
            return true;
        default:
            return false;
        }
    case Mari:
        switch (translationLang) {
        case Russian:
            return true;
        default:
            return false;
        }
    case HillMari:
        switch (translationLang) {
        case Russian:
            return true;
        default:
            return false;
        }
    case Dutch:
        switch (translationLang) {
        case English:
            return true;
        case Russian:
            return true;
        default:
            return false;
        }
    case Norwegian:
        switch (translationLang) {
        case English:
            return true;
        case Russian:
            return true;
        default:
            return false;
        }
    case Polish:
        switch (translationLang) {
        case Russian:
            return true;
        default:
            return false;
        }
    case Portuguese:
        switch (translationLang) {
        case English:
            return true;
        case Russian:
            return true;
        default:
            return false;
        }
    case Russian:
        switch (translationLang) {
        case Belarusian:
            return true;
        case Bulgarian:
            return true;
        case Czech:
            return true;
        case Danish:
            return true;
        case German:
            return true;
        case Greek:
            return true;
        case English:
            return true;
        case Spanish:
            return true;
        case Estonian:
            return true;
        case Finnish:
            return true;
        case French:
            return true;
        case Italian:
            return true;
        case Lithuanian:
            return true;
        case Latvian:
            return true;
        case Mari:
            return true;
        case HillMari:
            return true;
        case Dutch:
            return true;
        case Norwegian:
            return true;
        case Portuguese:
            return true;
        case Russian:
            return true;
        case Slovak:
            return true;
        case Swedish:
            return true;
        case Turkish:
            return true;
        case Tatar:
            return true;
        case Ukrainian:
            return true;
        default:
            return false;
        }
    case Slovak:
        switch (translationLang) {
        case English:
            return true;
        case Russian:
            return true;
        default:
            return false;
        }
    case Swedish:
        switch (translationLang) {
        case English:
            return true;
        case Russian:
            return true;
        default:
            return false;
        }
    case Turkish:
        switch (translationLang) {
        case German:
            return true;
        case English:
            return true;
        case Russian:
            return true;
        default:
            return false;
        }
    case Tatar:
        switch (translationLang) {
        case Russian:
            return true;
        default:
            return false;
        }
    case Ukrainian:
        switch (translationLang) {
        case English:
            return true;
        case Russian:
            return true;
        case Ukrainian:
            return true;
        default:
            return false;
        }
    default:
        return false;
    }
}

bool QOnlineTranslator::isSupportBingTranslation(QOnlineTranslator::Language lang)
{
    switch (lang) {
    case NoLanguage:
    case Albanian:
    case Amharic:
    case Armenian:
    case Azerbaijani:
    case Basque:
    case Bashkir:
    case Belarusian:
    case Cebuano:
    case Corsican:
    case Esperanto:
    case Frisian:
    case Galician:
    case Georgian:
    case Gujarati:
    case Hausa:
    case Hawaiian:
    case HillMari:
    case Igbo:
    case Irish:
    case Javanese:
    case Kannada:
    case Kazakh:
    case Khmer:
    case Kurdish:
    case Kyrgyz:
    case Lao:
    case Latin:
    case Luxembourgish:
    case Macedonian:
    case Malayalam:
    case Maori:
    case Marathi:
    case Mari:
    case Mongolian:
    case Myanmar:
    case Nepali:
    case Chichewa:
    case Papiamento:
    case Pashto:
    case Punjabi:
    case ScotsGaelic:
    case Sesotho:
    case Shona:
    case Sindhi:
    case Sinhala:
    case Somali:
    case Sundanese:
    case Tagalog:
    case Tajik:
    case Tatar:
    case Udmurt:
    case Uzbek:
    case Xhosa:
    case Yiddish:
    case Yoruba:
    case Zulu:
        return false;
    default:
        return true;
    }
}

bool QOnlineTranslator::isSupportBingTranslit(QOnlineTranslator::Language lang)
{
    switch (lang) {
    case Arabic:
    case Bengali:
    case SimplifiedChinese:
    case TraditionalChinese:
    case Gujarati:
    case Hebrew:
    case Hindi:
    case Japanese:
    case Kannada:
    case Malayalam:
    case Marathi:
    case Punjabi:
    case SerbianCyrillic:
    case Tamil:
    case Telugu:
    case Thai:
        return true;
    default:
        return false;
    }
}

bool QOnlineTranslator::isSupportBingDictionary(QOnlineTranslator::Language sourceLang, QOnlineTranslator::Language translationLang)
{
    // Bing support dictionary only to or from English (we don't need a dictionary for translation)
    if (sourceLang != English)
        return false;

    switch (translationLang) {
    case Afrikaans:
    case Arabic:
    case Bengali:
    case Bosnian:
    case Bulgarian:
    case Catalan:
    case SimplifiedChinese:
    case Croatian:
    case Czech:
    case Danish:
    case Dutch:
    case Estonian:
    case Finnish:
    case French:
    case German:
    case Greek:
    case HaitianCreole:
    case Hebrew:
    case Hindi:
    case Hmong:
    case Hungarian:
    case Icelandic:
    case Indonesian:
    case Italian:
    case Japanese:
    case Swahili:
    case Klingon:
    case Korean:
    case Latvian:
    case Lithuanian:
    case Malay:
    case Maltese:
    case Norwegian:
    case Persian:
    case Polish:
    case Portuguese:
    case Romanian:
    case Russian:
    case SerbianLatin:
    case Slovak:
    case Slovenian:
    case Spanish:
    case Swedish:
    case Tamil:
    case Thai:
    case Turkish:
    case Ukrainian:
    case Urdu:
    case Vietnamese:
    case Welsh:
        return true;
    default:
        return false;
    }
}

// Parse language from response language code
QOnlineTranslator::Language QOnlineTranslator::language(Engine engine, const QString &langCode)
{
    // Engine exceptions
    switch (engine) {
    case Google:
        if (langCode == "sr")
            return SerbianCyrillic;
        break;
    case Yandex:
        if (langCode == "sr")
            return SerbianCyrillic;
        if (langCode == "zn")
            return SimplifiedChinese;
        if (langCode == "jv")
            return Javanese;
        break;
    case Bing:
        if (langCode == "zh-Hans")
            return SimplifiedChinese;
        if (langCode == "zh-Hant")
            return TraditionalChinese;
        if (langCode == "mww")
            return Hmong;
        break;
    }

    // General case
    return static_cast<Language>(m_languageCodes.indexOf(langCode));
}

// Get split index of the text according to the limit
int QOnlineTranslator::getSplitIndex(const QString &untranslatedText, int limit)
{
    if (untranslatedText.size() < limit)
        return limit;

    int splitIndex = untranslatedText.lastIndexOf(". ", limit - 1);
    if (splitIndex != -1)
        return splitIndex + 1;

    splitIndex = untranslatedText.lastIndexOf(" ", limit - 1);
    if (splitIndex != -1)
        return splitIndex + 1;

    splitIndex = untranslatedText.lastIndexOf("\n", limit - 1);
    if (splitIndex != -1)
        return splitIndex + 1;

    // Non-breaking space
    splitIndex = untranslatedText.lastIndexOf(0x00a0, limit - 1);
    if (splitIndex != -1)
        return splitIndex + 1;

    // If the text has not passed any check and is most likely garbage
    return limit;
}

void QOnlineTranslator::resetData()
{
    m_translation.clear();
    m_translationTranslit.clear();
    m_sourceTranslit.clear();
    m_sourceTranscription.clear();
    m_translationOptions.clear();
    m_examples.clear();
}

bool QOnlineTranslator::isExamplesEnabled() const
{
    return m_examplesEnabled;
}

void QOnlineTranslator::setExamplesEnabled(bool enable)
{
    m_examplesEnabled = enable;
}

bool QOnlineTranslator::isTranslationOptionsEnabled() const
{
    return m_translationOptionsEnabled;
}

void QOnlineTranslator::setTranslationOptionsEnabled(bool enable)
{
    m_translationOptionsEnabled = enable;
}

bool QOnlineTranslator::isSourceTranscriptionEnabled() const
{
    return m_sourceTranscriptionEnabled;
}

void QOnlineTranslator::setSourceTranscriptionEnabled(bool enable)
{
    m_sourceTranscriptionEnabled = enable;
}

bool QOnlineTranslator::isTranslationTranslitEnabled() const
{
    return m_translationTranslitEnabled;
}

void QOnlineTranslator::setTranslationTranslitEnabled(bool enable)
{
    m_translationTranslitEnabled = enable;
}

bool QOnlineTranslator::isSourceTranslitEnabled() const
{
    return m_sourceTranslitEnabled;
}

void QOnlineTranslator::setSourceTranslitEnabled(bool enable)
{
    m_sourceTranslitEnabled = enable;
}
