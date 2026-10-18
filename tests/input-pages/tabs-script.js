let count = 0;
console.log('TABS_TEST external script loaded');
document.getElementById('count').onclick = () => {
  document.getElementById('state').textContent = 'Count ' + ++count;
  console.log('TABS_TEST count=' + count + ' text=' + document.getElementById('text').value);
};
document.addEventListener('visibilitychange', () => console.log('TABS_TEST visibility=' + document.visibilityState));

window.addEventListener('scroll', () => console.log('TABS_TEST scroll=' + window.scrollY));
