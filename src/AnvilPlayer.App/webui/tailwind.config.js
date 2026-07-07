/** @type {import('tailwindcss').Config} */
export default {
  content: ['./index.html', './src/**/*.{ts,tsx}'],
  theme: {
    extend: {
      colors: {
        accent: '#df765f',
        ink: '#08090d'
      }
    }
  },
  plugins: []
}
